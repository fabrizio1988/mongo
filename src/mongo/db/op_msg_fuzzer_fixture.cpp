/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/op_msg_fuzzer_fixture.h"

#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_access_method_factory_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_state_factory_standalone.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/storage/storage_engine_mock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/transport/service_entry_point_impl.h"

namespace mongo {
OpMsgFuzzerFixture::OpMsgFuzzerFixture(bool skipGlobalInitializers) {
    if (!skipGlobalInitializers) {
        auto ret = runGlobalInitializers(std::vector<std::string>{});
        invariant(ret.isOK());
    }

    setGlobalServiceContext(ServiceContext::make());
    _session = _transportLayer.createSession();

    _serviceContext = getGlobalServiceContext();
    _serviceContext->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointMongod>(_serviceContext));

    _clientStrand = ClientStrand::make(_serviceContext->makeClient("test", _session));

    _serviceContext->setStorageEngine(std::make_unique<StorageEngineMock>());

    CollectionShardingStateFactory::set(
        _serviceContext,
        std::make_unique<CollectionShardingStateFactoryStandalone>(_serviceContext));
    DatabaseHolder::set(_serviceContext, std::make_unique<DatabaseHolderImpl>());
    IndexAccessMethodFactory::set(_serviceContext,
                                  std::make_unique<IndexAccessMethodFactoryImpl>());
    Collection::Factory::set(_serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());

    auto localExternalState = std::make_unique<AuthzManagerExternalStateMock>();
    _externalState = localExternalState.get();

    auto localAuthzManager =
        std::make_unique<AuthorizationManagerImpl>(_serviceContext, std::move(localExternalState));
    _authzManager = localAuthzManager.get();
    _externalState->setAuthorizationManager(_authzManager);
    _authzManager->setAuthEnabled(true);

    AuthorizationManager::set(_serviceContext, std::move(localAuthzManager));

    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(_serviceContext);
    invariant(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(getGlobalServiceContext(), std::move(replCoord));
}

int OpMsgFuzzerFixture::testOneInput(const char* Data, size_t Size) {
    if (Size < sizeof(MSGHEADER::Value)) {
        return 0;
    }

    auto clientGuard = _clientStrand->bind();
    auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());
    VectorClockMutable::get(_serviceContext)->tickClusterTimeTo(kInMemoryLogicalTime);

    int new_size = Size + sizeof(int);
    auto sb = SharedBuffer::allocate(new_size);
    memcpy(sb.get(), &new_size, sizeof(int));
    memcpy(sb.get() + sizeof(int), Data, Size);
    Message msg(std::move(sb));

    try {
        _serviceContext->getServiceEntryPoint()->handleRequest(opCtx.get(), msg).get();
    } catch (const AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    return 0;
}
}  // namespace mongo