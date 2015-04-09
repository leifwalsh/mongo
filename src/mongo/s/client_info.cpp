// @file s/client_info.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client_info.h"

#include <boost/thread.hpp>

#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
    boost::thread_specific_ptr<ClientInfo> tlInfo;
}  // namespace

    ClientInfo* ClientInfo::create(ServiceContext* serviceContext,
                                   AbstractMessagingPort* messagingPort) {
        ClientInfo * info = tlInfo.get();
        massert(16472, "A ClientInfo already exists for this thread", !info);
        info = new ClientInfo(serviceContext, messagingPort);
        info->setAuthorizationSession(getGlobalAuthorizationManager()->makeAuthorizationSession());
        tlInfo.reset( info );
        return info;
    }

    ClientInfo* ClientInfo::get() {
        ClientInfo* info = tlInfo.get();
        //fassert(16483, info);
        if (!info) {
            return create(getGlobalServiceContext(), nullptr);
        }
        return info;
    }

    bool ClientInfo::exists() {
        return tlInfo.get();
    }

    ClientInfo::ClientInfo(ServiceContext* serviceContext, AbstractMessagingPort* messagingPort) :
        ClientBasic(serviceContext, messagingPort) {}

    ClientBasic* ClientBasic::getCurrent() {
        return ClientInfo::get();
    }

} // namespace mongo
