/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specic language governing permissions and
 * limitations under the License.
 */

#ifndef MEDIAPROVIDER_FUSE_MEDIAPROVIDERWRAPPER_H_
#define MEDIAPROVIDER_FUSE_MEDIAPROVIDERWRAPPER_H_

#include <jni.h>
#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "libfuse_jni/ReaddirHelper.h"
#include "libfuse_jni/RedactionInfo.h"

namespace mediaprovider {
namespace fuse {

/**
 * Type describing a JNI task, sent to the JNI thread.
 * The function only takes JNIEnv because that's the parameter that JNI thread
 * must provide. The rest of the arguments can be captured by the lambda,
 * the return value should be captured by reference.
 */
typedef std::function<void(JNIEnv*)> JniTask;

/**
 * Class that wraps MediaProvider.java and all of the needed JNI calls to make
 * interaction with MediaProvider easier.
 */
class MediaProviderWrapper final {
  public:
    MediaProviderWrapper(JNIEnv* env, jobject media_provider);
    ~MediaProviderWrapper();

    /**
     * Computes and returns the RedactionInfo for a given FD and UID.
     *
     * @param uid UID of the app requesting the read
     * @param fd FD of the requested file
     * @return RedactionInfo on success, nullptr on failure to calculate
     * redaction ranges (e.g. exception was thrown in Java world)
     */
    std::unique_ptr<RedactionInfo> GetRedactionInfo(const std::string& path, uid_t uid);

    /**
     * Inserts a new entry for the given path and UID.
     *
     * @param path the path of the file to be created
     * @param uid UID of the calling app
     * @return 0 if the operation succeeded,
     * or negated errno error code if operation fails.
     */
    int InsertFile(const std::string& path, uid_t uid);

    /**
     * Delete the file denoted by the given path on behalf of the given UID.
     *
     * @param path the path of the file to be deleted
     * @param uid UID of the calling app
     * @return 0 upon success, or negated errno error code if operation fails.
     */
    int DeleteFile(const std::string& path, uid_t uid);

    /**
     * Gets directory entries for given relative path from MediaProvider database.
     *
     * @param uid UID of the calling app.
     * @param path Relative path of the directory.
     * @param dirp pointer to directory stream
     * @return DirectoryEntries with list of directory entries on success,
     * DirectoryEntries with an empty list if directory path is unknown to MediaProvider
     * or no directory entries are visible to the calling app.
     */
    std::vector<std::shared_ptr<DirectoryEntry>> GetDirectoryEntries(uid_t uid,
                                                                     const std::string& path,
                                                                     DIR* dirp);

    /**
     * Determines if the given UID is allowed to open the file denoted by the given path.
     *
     * @param path the path of the file to be opened
     * @param uid UID of the calling app
     * @param for_write specifies if the file is to be opened for write
     * @return 0 upon success or negated errno value upon failure.
     */
    int IsOpenAllowed(const std::string& path, uid_t uid, bool for_write);

    /**
     * Potentially triggers a scan of the file before closing it and reconciles it with the
     * MediaProvider database.
     *
     * @param path the path of the file to be scanned
     */
    void ScanFile(const std::string& path);

    /**
     * Determines if the given UID is allowed to create a directory with the given path.
     *
     * @param path the path of the directory to be created
     * @param uid UID of the calling app
     * @return 0 if it's allowed, or negated errno error code if operation isn't allowed.
     */
    int IsCreatingDirAllowed(const std::string& path, uid_t uid);

    /**
     * Determines if the given UID is allowed to delete the directory with the given path.
     *
     * @param path the path of the directory to be deleted
     * @param uid UID of the calling app
     * @return 0 if it's allowed, or negated errno error code if operation isn't allowed.
     */
    int IsDeletingDirAllowed(const std::string& path, uid_t uid);

    /**
     * Determines if the given UID is allowed to open the directory with the given path.
     *
     * @param path the path of the directory to be opened
     * @param uid UID of the calling app
     * @return 0 if it's allowed, or negated errno error code if operation isn't allowed.
     */
    int IsOpendirAllowed(const std::string& path, uid_t uid);

  private:
    jclass media_provider_class_;
    jobject media_provider_object_;
    /** Cached MediaProvider method IDs **/
    jmethodID mid_get_redaction_ranges_;
    jmethodID mid_insert_file_;
    jmethodID mid_delete_file_;
    jmethodID mid_is_open_allowed_;
    jmethodID mid_scan_file_;
    jmethodID mid_is_dir_op_allowed_;
    jmethodID mid_is_opendir_allowed_;
    jmethodID mid_get_directory_entries_;
    /**
     * All JNI calls are delegated to this thread
     */
    std::thread jni_thread_;
    /**
     * jniThread loops until d'tor is called, waiting for a notification on condition_variable to
     * perform a task
     */
    std::condition_variable pending_task_cond_;
    /**
     * Communication with jniThread is done through this JniTasks queue.
     */
    std::queue<JniTask> jni_tasks_;
    /**
     * Threads can post a JNI task if and only if this is true.
     */
    std::atomic<bool> jni_tasks_welcome_;
    /**
     * JNI thread keeps running until it receives a task that sets this flag to true.
     */
    std::atomic<bool> request_terminate_jni_thread_;
    /**
     * All member variables prefixed with jni should be guarded by this lock.
     */
    std::mutex jni_task_lock_;
    /**
     * Auxiliary for caching MediaProvider methods.
     */
    jmethodID CacheMethod(JNIEnv* env, const char method_name[], const char signature[],
                          bool is_static);
    /**
     * Main loop for the JNI thread.
     */
    void JniThreadLoop(JavaVM* jvm);
    /**
     * Mechanism for posting JNI tasks and waiting until they're done.
     * @return true if task was successfully posted and performed, false otherwise.
     */
    bool PostAndWaitForTask(const JniTask& t);
    /**
     * Mechanism for posting JNI tasks that don't have a response.
     * There's no guarantee that the task will be actually performed.
     */
    void PostAsyncTask(const JniTask& t);
};

}  // namespace fuse
}  // namespace mediaprovider

#endif  // MEDIAPROVIDER_FUSE_MEDIAPROVIDERWRAPPER_H_