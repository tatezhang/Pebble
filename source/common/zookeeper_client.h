/*
 * Tencent is pleased to support the open source community by making Pebble available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 * Licensed under the MIT License (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 *
 */


#ifndef COMMON_ZOOKEEPER_CLIENT_ST_H_
#define COMMON_ZOOKEEPER_CLIENT_ST_H_

#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <set>
#include <string>
#include <tr1/functional>
#include "source/common/pebble_common.h"
#include "thirdparty/zookeeper/include/zookeeper.h"
#include "thirdparty/zookeeper/include/zookeeper_log.h"
#include "source/log/log.h"

typedef cxx::function<void(int rc)> ZkVoidCompletionCb;

typedef cxx::function<void(int rc, const Stat *stat)> ZkStatCompletionCb;

typedef cxx::function<void(int rc, const char *value)> ZkStringCompletionCb;

typedef cxx::function<void(int rc, const String_vector *strings,
                           const Stat *stat)> ZkStringsCompletionCb;

typedef cxx::function<void(int rc, ACL_vector *acl,
                           Stat *stat)> ZkAclCompletionCb;

typedef cxx::function<void(int rc, const char *value, int value_len,
                           const Stat *stat)> ZkDataCompletionCb;

enum ZookeeperErrorCode {
    kZK_OK = 0, /*!< Everything is OK */

    /** System and server-side errors.
     * This is never thrown by the server, it shouldn't be used other than
     * to indicate a range. Specifically error codes greater than this
     * value, but lesser than {@link #ZAPIERROR}, are system errors. */

    kZK_SYSTEMERROR = -1,
    kZK_RUNTIMEINCONSISTENCY = -2,/*!< A runtime inconsistency was found */
    kZK_DATAINCONSISTENCY = -3, /*!< A data inconsistency was found */
    kZK_CONNECTIONLOSS = -4, /*!< Connection to the server has been lost */
    /*!< Error while marshalling or unmarshalling data */
    kZK_MARSHALLINGERROR = -5,
    kZK_UNIMPLEMENTED = -6, /*!< Operation is unimplemented */
    kZK_OPERATIONTIMEOUT = -7, /*!< Operation timeout */
    kZK_BADARGUMENTS = -8, /*!< Invalid arguments */
    kZK_INVALIDSTATE = -9, /*!< Invliad zhandle state */
    kZK_DNSFAILURE = -10, /*!< Error occurs when dns lookup  */

    kZK_APIERROR = -100,
    kZK_NONODE = -101, /*!< Node does not exist */
    kZK_NOAUTH = -102, /*!< Not authenticated */
    kZK_BADVERSION = -103, /*!< Version conflict */
    /*!< Ephemeral nodes may not have children */
    kZK_NOCHILDRENFOREPHEMERALS = -108,
    kZK_NODEEXISTS = -110, /*!< The node already exists */
    kZK_NOTEMPTY = -111, /*!< The node has children */
    /*!< The session has been expired by the server */
    kZK_SESSIONEXPIRED = -112,
    kZK_INVALIDCALLBACK = -113, /*!< Invalid callback specified */
    kZK_INVALIDACL = -114, /*!< Invalid ACL specified */
    kZK_AUTHFAILED = -115, /*!< Client authentication failed */
    kZK_CLOSING = -116, /*!< ZooKeeper is closing */
    kZK_NOTHING = -117, /*!< (not error) no server responses to process */
    /*!< session moved to another server, so operation is ignored */
    kZK_SESSIONMOVED = -118,
    kZK_NOQUOTA = -119, /*!< quota is not enough. */
    kZK_SERVEROVERLOAD = -120, /*!< server overload. */

    kZK_ENCRYPTFAILED = -200, /*!< Digest Encrypt failed. */
};

struct EphemeralNodeInfo
{
    EphemeralNodeInfo();
    ~EphemeralNodeInfo();
    EphemeralNodeInfo(const EphemeralNodeInfo& rhs);
    EphemeralNodeInfo& operator = (const EphemeralNodeInfo& rhs);
    bool operator < (const EphemeralNodeInfo& rhs) const
    {
        return (_path.compare(rhs._path) < 0);
    }

    std::string _path;
    std::string _value;
    ACL_vector _acl_vec;
};

/// @brief zk客户端的c++封装
/// @note 本封装主要用于配置管理，对于session恢复为恢复所有的鉴权、watch和临时节点。
///       watch不会因为已经触发过了而不恢复；未调用接口删除的临时节点按创建时的信息恢复。
class ZookeeperClient {
public:
    ZookeeperClient();

    ~ZookeeperClient();

    // init
    int Init(const std::string& host, int time_out, std::string zk_path);

    // connect zookeeper asynchronously
    int AConnect();

    // DO NOT call by user
    void OnConnected();

    // connect zookeeper asynchronously
    int Connect();

    // create a node asynchronously
    int ACreate(const char* path, const char* value, int value_length,
                const ACL_vector* acl, int flags, ZkStringCompletionCb cob);

    // create a node synchronously
    int Create(const char* path, const char* value, int value_length,
               const ACL_vector* acl, int flags);

    // gets the data associated with a node asynchronously
    int AGet(const char* path, int watch, ZkDataCompletionCb cob);

    // gets the data associated with a node synchronously
    int Get(const char* path, int watch, char* buffer, int* length, Stat* stat);

    // sets the data with a node asynchronously
    int ASet(const char* path, const char* buffer, int buflen,
             int version, ZkStatCompletionCb cob);

    // sets the data with a node synchronously
    int Set(const char* path, const char* buffer, int buflen, int version, Stat* stat);

    // dels the data with a node asynchronously
    int ADelete(const char* path, int version, ZkVoidCompletionCb cob);

    // dels the data with a node synchronously
    int Delete(const char* path, int version);

    // checks the existence of a node in zookeeper asynchronously
    int AExists(const char* path, int watch, ZkStatCompletionCb cob);

    // checks the existence of a node in zookeeper synchronously
    int Exists(const char* path, int watch, Stat* stat);

    // checks the existence of a node in zookeeper asynchronously by user watcher
    int AWExists(const char* path, watcher_fn watcher, void* watcherCtx,
                 ZkStatCompletionCb cob);

    // checks the existence of a node in zookeeper synchronously by user watcher
    int WExists(const char* path, watcher_fn watcher, void* watcherCtx,
                Stat* stat);

    // lists the children of a node asynchronously
    int AGetChildren(const char* path, int watch, ZkStringsCompletionCb cob);

    // lists the children of a node synchronously
    int GetChildren(const char* path, int watch,
                    String_vector* children, Stat* stat);

    // gets the acl associated with a node asynchronously
    int AGetAcl(const char *path, ZkAclCompletionCb cob);

    // gets the acl associated with a node synchronously
    int GetAcl(const char *path, ACL_vector* acl);

    // 异步驱动更新
    /// @return -1 不需要做任何动作
    /// @return 0 有更新动作
    int Update(bool is_block = false);

    // close the zookeeper handle and free up any resources
    int Close(bool is_clean = false);

    // specify application credentials
    int AddDigestAuth(const std::string& digest_auth);

    // Base64(sha1(gameid:gamekey))
    static std::string DigestEncrypt(const std::string& id_passwd);

    void SetWatchCallback(std::tr1::function<void(int, const char*)> cb) {
        m_watch_cb = cb;
    }

    std::tr1::function<void(int, const char*)> GetWatchCallback() {
        return m_watch_cb;
    }

    zhandle_t* zk_handle() {return m_zk_handle;}

    static void EphemeralNodeCreateCallback(int rc, const char *value, const void* data);

private:
    std::string m_zk_host;
    int m_time_out;
    std::string m_zk_path;

    zhandle_t* m_zk_handle;

    std::set<std::string> m_auths_set;
    std::tr1::function<void(int, const char*)> m_watch_cb;
    std::set<std::string> m_get_watch;
    std::set<std::string> m_get_child_watch;
    std::set<std::string> m_exist_watch;
    // 保存临时节点信息，在session expired恢复后自动恢复临时节点
    std::set<EphemeralNodeInfo> m_ephemeral_node;
};

#endif  // COMMON_ZOOKEEPER_CLIENT_ST_H_
