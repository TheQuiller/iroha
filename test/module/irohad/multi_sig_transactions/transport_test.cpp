/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include "module/irohad/multi_sig_transactions/mst_mocks.hpp"
#include "module/irohad/multi_sig_transactions/mst_test_helpers.hpp"
#include "multi_sig_transactions/state/mst_state.hpp"
#include "multi_sig_transactions/transport/mst_transport_grpc.hpp"

using namespace iroha::network;
using namespace iroha::model;

using ::testing::_;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;

/**
 * @brief Sends data over MstTransportGrpc (MstState and Peer objects) and
 * receives them. When received deserializes them end ensures that deserialized
 * objects equal to objects before sending.
 *
 * @given Initialized transport
 * AND MstState for transfer
 * @when Send state via transport
 * @then Assume that received state same as sent
 */
TEST(TransportTest, SendAndReceive) {
  auto transport = std::make_shared<MstTransportGrpc>();
  auto notifications = std::make_shared<iroha::MockMstTransportNotification>();
  transport->subscribe(notifications);

  std::mutex mtx;
  std::condition_variable cv;
  ON_CALL(*notifications, onNewState(_, _))
      .WillByDefault(
          InvokeWithoutArgs(&cv, &std::condition_variable::notify_one));

  auto state = iroha::MstState::empty();
  state += makeTestBatch(txBuilder(1, iroha::time::now(), makeKey(), 3));
  state += makeTestBatch(txBuilder(1, iroha::time::now(), makeKey(), 4));
  state += makeTestBatch(txBuilder(1, iroha::time::now(), makeKey(), 5));
  state += makeTestBatch(txBuilder(1, iroha::time::now(), makeKey(), 5));

  std::unique_ptr<grpc::Server> server;

  grpc::ServerBuilder builder;
  int port = 0;
  std::string addr = "localhost:";
  builder.AddListeningPort(
      addr + "0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(transport.get());
  server = builder.BuildAndStart();
  ASSERT_TRUE(server);
  ASSERT_NE(port, 0);

  std::shared_ptr<shared_model::interface::Peer> peer =
      makePeer(addr + std::to_string(port), "abcdabcdabcdabcdabcdabcdabcdabcd");
  // we want to ensure that server side will call onNewState()
  // with same parameters as on the client side
  EXPECT_CALL(*notifications, onNewState(_, state))
      .WillOnce(Invoke([&peer](auto &p, auto) { EXPECT_EQ(*p, *peer); }));

  transport->sendState(*peer, state);
  std::unique_lock<std::mutex> lock(mtx);
  cv.wait_for(lock, std::chrono::milliseconds(100));

  server->Shutdown();
}
