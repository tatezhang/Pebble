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

#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <string.h>

#include "client/pebble_client.h"
#include "common/coroutine.h"
#include "common/log.h"
#include "common/string_utility.h"
#include "common/time_utility.h"
#include "common/timer.h"
#include "framework/event_handler.inh"
#include "framework/message.h"
#include "framework/pebble_rpc.h"
#include "framework/register_error.h"
#include "framework/session.h"
#include "framework/stat.h"
#include "framework/stat_manager.h"
#include "pebble_version.inh"
#include "version.inh"


namespace pebble {


const char* GetVersion() {
    static char version[256] = {0};

    snprintf(version, sizeof(version), "Pebble : %s %s %s",
        PebbleVersion::GetVersion().c_str(), __TIME__, __DATE__);

    return version;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////

PebbleClient::PebbleClient() {
    m_coroutine_schedule = NULL;
    m_stat_manager       = NULL;
    m_timer              = NULL;
    m_stat_timer_ms      = 1000;
    m_rpc_event_handler  = NULL;
    m_session_mgr        = NULL;

    for (int32_t i = 0; i < kNAMING_BUTT; ++i) {
        m_naming_array[i] = NULL;
    }

    for (int32_t i = 0; i < kPROTOCOL_TYPE_BUTT; ++i) {
        m_processor_array[i] = NULL;
    }
}

PebbleClient::~PebbleClient() {
    // delete的原则: 如果出现重复delete说明逻辑实现有问题，通过crash暴露出来，赋NULL可能掩盖问题
    for (cxx::unordered_map<std::string, Router*>::iterator it = m_router_map.begin();
        it != m_router_map.end(); ++it) {
        delete it->second;
    }

    for (int32_t i = 0; i < kNAMING_BUTT; ++i) {
        delete m_naming_array[i];
    }

    for (int32_t i = 0; i < kPROTOCOL_TYPE_BUTT; ++i) {
        delete m_processor_array[i];
    }

    delete m_rpc_event_handler;
    delete m_stat_manager;
    delete m_coroutine_schedule;
    delete m_timer;
    delete m_session_mgr;
}

int32_t PebbleClient::Init() {
    // 具体模块初始化失败的详细信息应该在具体的init中输出，这里统一返回
    #define CHECK_RETURN(ret) \
        if (ret) { \
            return -1; \
        }

    RegisterErrorString();

    InitLog();

    PLOG_INFO("%s", m_options.ToString().c_str());

    int32_t ret = InitTimer();
    CHECK_RETURN(ret);

    ret = InitCoSchedule();
    CHECK_RETURN(ret);

    ret = InitStat();
    CHECK_RETURN(ret);

	MessageCallbacks cbs;
	using namespace cxx::placeholders;
	cbs._on_message = cxx::bind(&PebbleClient::OnMessage, this, _1, _2, _3);
	cbs._on_peer_connected = cxx::bind(&PebbleClient::OnPeerConnected, this, _1, _2);
	cbs._on_peer_closed = cxx::bind(&PebbleClient::OnPeerClosed, this, _1, _2);
	cbs._on_closed = cxx::bind(&PebbleClient::OnClosed, this, _1);
    ret = Message::Init(cbs);
    CHECK_RETURN(ret);

	cxx::shared_ptr<RouterFactory> router_factory(new RouterFactory());
	ret = SetRouterFactory(kROUTER_DEFAULT, router_factory);
	CHECK_RETURN(ret);

    signal(SIGPIPE, SIG_IGN);

    return 0;
}

int64_t PebbleClient::Connect(const std::string &url) {
    int64_t handle = Message::Connect(url);
    PLOG_IF_ERROR(handle < 0, "connect %s failed(%ld)", url.c_str(), handle);
    return handle;
}

int32_t PebbleClient::Close(int64_t handle) {
    int32_t ret = Message::Close(handle);
    PLOG_IF_ERROR(ret != 0, "close %ld failed", handle);
    return ret;
}

int32_t PebbleClient::Attach(int64_t handle, IProcessor* processor) {
    if (NULL == processor) {
        PLOG_ERROR("param processor is NULL");
        return -1;
    }

    m_processor_map[handle] = processor;
    return 0;
}

int32_t PebbleClient::Attach(Router* router, IProcessor* processor) {
    if (NULL == router || NULL == processor) {
        PLOG_ERROR("param error. &router = %p &processor = %p", router, processor);
        return -1;
    }

    router->SetOnAddressChanged(cxx::bind(&PebbleClient::OnRouterAddressChanged, this,
        router, cxx::placeholders::_1, processor));
    return 0;
}

PebbleRpc* PebbleClient::GetPebbleRpc(ProtocolType protocol_type) {
    if (protocol_type < kPEBBLE_RPC_BINARY || protocol_type > kPEBBLE_RPC_PROTOBUF) {
        PLOG_ERROR("param protocol_type invalid(%d)", protocol_type);
        return NULL;
    }

    if (m_processor_array[protocol_type] != NULL) {
        return dynamic_cast<PebbleRpc*>(m_processor_array[protocol_type]);
    }

    if (!m_rpc_event_handler) {
        m_rpc_event_handler = new RpcEventHandler();
        static_cast<RpcEventHandler*>(m_rpc_event_handler)->Init(m_stat_manager);
    }

    CodeType rpc_code_type = kCODE_BUTT;
    switch (protocol_type) {
        case kPEBBLE_RPC_BINARY:
            rpc_code_type = kCODE_BINARY;
            break;

        case kPEBBLE_RPC_JSON:
            rpc_code_type = kCODE_JSON;
            break;

        case kPEBBLE_RPC_PROTOBUF:
            rpc_code_type = kCODE_PB;
            break;

        default:
            PLOG_FATAL("unsupport protocol type %d", protocol_type);
            return NULL;
    }

    PebbleRpc* rpc_instance = new PebbleRpc(rpc_code_type, m_coroutine_schedule);
    rpc_instance->SetSendFunction(Message::Send, Message::SendV);
    rpc_instance->SetEventHandler(m_rpc_event_handler);
    m_processor_array[protocol_type] = rpc_instance;

    return rpc_instance;
}

Naming* PebbleClient::GetNaming(NamingType naming_type) {
    if (m_naming_array[naming_type] != NULL) {
        return m_naming_array[naming_type];
    }

    cxx::shared_ptr<NamingFactory> factory = GetNamingFactory(naming_type);
    if (!factory) {
        PLOG_ERROR("unsupport naming_type %d", naming_type);
        return NULL;
    }

    m_naming_array[naming_type] = factory->GetNaming(
        m_options._bc_zk_host, m_options._bc_zk_timeout_ms);

    return m_naming_array[naming_type];
}

Router* PebbleClient::GetRouter(const std::string& name, RouterType router_type) {
    if (name.empty()) {
        PLOG_ERROR("name is empty");
        return NULL;
    }

    cxx::unordered_map<std::string, Router*>::iterator it = m_router_map.find(name);
    if (it != m_router_map.end()) {
        return it->second;
    }

    cxx::shared_ptr<RouterFactory> factory = GetRouterFactory(router_type);
    if (!factory) {
        PLOG_ERROR("unsupport router_type %d", router_type);
        return NULL;
    }

    Router* router = factory->GetRouter(name);
    // 这里默认使用tbuspp名字服务，用户可以重新Init设置其他名字服务
    int32_t ret = router->Init(GetNaming());
    if (ret != 0) {
        PLOG_ERROR("router %s init failed(%d)", name.c_str(), ret);
    }

    m_router_map[name] = router;
    return router;
}

int32_t PebbleClient::Update() {
    int32_t num = 0;

    int64_t old = TimeUtility::GetCurrentMS();

	num += Message::Update();

    for (int32_t i = 0; i < kNAMING_BUTT; ++i) {
        if (m_naming_array[i]) {
            num += m_naming_array[i]->Update();
        }
    }

    for (int32_t i = 0; i < kPROTOCOL_TYPE_BUTT; ++i) {
        if (m_processor_array[i]) {
            m_processor_array[i]->Update();
        }
    }

    if (m_timer) {
        num += m_timer->Update();
    }

    if (m_session_mgr) {
        num += m_session_mgr->CheckTimeout();
    }

    if (m_stat_manager) {
        num += m_stat_manager->Update();
        m_stat_manager->GetStat()->AddResourceItem("_loop", TimeUtility::GetCurrentMS() - old);
    }

    return num;
}

int32_t PebbleClient::OnMessage(const uint8_t* msg, uint32_t msg_len, MsgExternInfo* info) {
    cxx::unordered_map<int64_t, IProcessor*>::iterator it = m_processor_map.find(info->_self_handle);
    if (m_processor_map.end() == it) {
        PLOG_ERROR("handle(%ld) not attach a processor remote(%ld)", info->_self_handle, info->_remote_handle);
    } else {
	    it->second->OnMessage(m_last_msg_info._remote_handle, msg, msg_len, info, 0);
	}

    return 1;
}

int32_t PebbleClient::OnPeerConnected(int64_t local_handle, int64_t peer_hanlde) {
	return 0;
}
	
int32_t PebbleClient::OnPeerClosed(int64_t local_handle, int64_t peer_hanlde) {
	return 0;
}
	
int32_t PebbleClient::OnClosed(int64_t handle) {
	return 0;
}

void PebbleClient::InitLog() {
    Log::Instance().SetOutputDevice(m_options._log_device);
    Log::Instance().SetLogPriority(m_options._log_priority);
    Log::Instance().SetMaxFileSize(m_options._log_file_size_MB);
    Log::Instance().SetMaxRollNum(m_options._log_roll_num);
    Log::Instance().SetFilePath(m_options._log_path);

    // Log::EnableCrashRecord();
}

int32_t PebbleClient::InitCoSchedule() {
    if (m_coroutine_schedule) {
        return 0;
    }

    m_coroutine_schedule = new CoroutineSchedule();
    int32_t ret = m_coroutine_schedule->Init(GetTimer(), m_options._co_stack_size_bytes);
    if (ret != 0) {
        delete m_coroutine_schedule;
        m_coroutine_schedule = NULL;
        PLOG_ERROR("co schedule init failed(%d)", ret);
        return -1;
    }

    return 0;
}

int32_t PebbleClient::InitStat() {
    if (!m_stat_manager) {
        m_stat_manager = new StatManager();
    }

    m_stat_manager->SetReportCycle(m_options._stat_report_cycle_s);
    m_stat_manager->SetGdataParameter(m_options._stat_report_to_gdata,
        m_options._gdata_id, m_options._gdata_log_id);

    return m_stat_manager->Init(m_options._app_id, m_options._app_unit_id,
        m_options._app_program_id, m_options._app_instance_id, m_options._gdata_log_path);
}

int32_t PebbleClient::InitTimer() {
    if (!m_timer) {
        m_timer = new SequenceTimer();
    }

    TimeoutCallback on_stat_timeout = cxx::bind(&PebbleClient::OnStatTimeout, this);
    int64_t ret = m_timer->StartTimer(m_stat_timer_ms, on_stat_timeout);
    if (ret < 0) {
        PLOG_ERROR("start stat timer failed(%d:%s)", ret, m_timer->GetLastError());
        return -1;
    }

    return 0;
}

int32_t PebbleClient::OnStatTimeout() {
    Stat* stat = m_stat_manager->GetStat();
    StatCoroutine(stat);
    StatProcessorResource(stat);

    return m_stat_timer_ms;
}

void PebbleClient::OnRouterAddressChanged(Router* router,
    const std::vector<int64_t>& handles, IProcessor* processor) {

    cxx::unordered_map<Router*, std::vector<int64_t> >::iterator it =
        m_router_handle_map.find(router);
    if (it == m_router_handle_map.end()) {
        for (std::vector<int64_t>::const_iterator hit = handles.begin(); hit != handles.end(); ++hit) {
            Attach(*hit, processor);
        }
        m_router_handle_map[router] = handles;
        return;
    }

    // 采取简单策略，先把老的都detach，再attach新的
    for (std::vector<int64_t>::iterator oit = it->second.begin(); oit != it->second.end(); ++oit) {
        Detach(*oit);
    }
    for (std::vector<int64_t>::const_iterator nit = handles.begin(); nit != handles.end(); ++nit) {
        Attach(*nit, processor);
    }
    it->second = handles;
}

void PebbleClient::StatCoroutine(Stat* stat) {
    stat->AddResourceItem("_coroutine", m_coroutine_schedule->Size());
}

void PebbleClient::StatProcessorResource(Stat* stat) {
    cxx::unordered_map<std::string, int64_t> resource;
    cxx::unordered_map<std::string, int64_t>::iterator it;

    for (int32_t i = 0; i < kPROTOCOL_TYPE_BUTT; ++i) {
        if (NULL == m_processor_array[i]) {
            continue;
        }

        // 统计Processor使用动态资源情况
        resource.clear();
        m_processor_array[i]->GetResourceUsed(&resource);
        for (it = resource.begin(); it != resource.end(); ++it) {
            stat->AddResourceItem(it->first, it->second);
        }
    }
}

SessionMgr* PebbleClient::GetSessionMgr() {
    if (!m_session_mgr) {
        m_session_mgr = new SessionMgr();
    }
    return m_session_mgr;
}

Stat* PebbleClient::GetStat() {
    if (m_stat_manager) {
        return m_stat_manager->GetStat();
    }
    return NULL;
}

int32_t PebbleClient::MakeCoroutine(const cxx::function<void()>& routine) {
    if (!m_coroutine_schedule) {
        PLOG_ERROR("coroutine schedule is null");
        return -1;
    }

    if (!routine) {
        PLOG_ERROR("task is null");
        return -1;
    }

    CommonCoroutineTask* task = m_coroutine_schedule->NewTask<pebble::CommonCoroutineTask>();
    if (NULL == task) {
        PLOG_ERROR("new co task failed");
        return -1;
    }

    task->Init(routine);
    int64_t coid = task->Start(true);

    return coid < 0 ? -1 : 0;
}

MsgExternInfo* PebbleClient::GetLastMessageInfo() {
    return &m_last_msg_info;
}

int32_t PebbleClient::Detach(int64_t handle) {
    int num = m_processor_map.erase(handle);
    return num == 1 ? 0 : -1;
}


}  // namespace pebble

