// Copyright 2023 Northern.tech AS
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.

#include <api/auth.hpp>

#include <string>
#include <iostream>

#include <gtest/gtest.h>

#include <common/error.hpp>
#include <common/events.hpp>
#include <common/expected.hpp>
#include <common/dbus.hpp>
#include <common/http.hpp>
#include <common/io.hpp>
#include <common/log.hpp>
#include <common/path.hpp>
#include <common/processes.hpp>
#include <common/testing.hpp>

using namespace std;

namespace auth = mender::api::auth;
namespace dbus = mender::common::dbus;
namespace error = mender::common::error;
namespace events = mender::common::events;
namespace expected = mender::common::expected;
namespace http = mender::http;
namespace io = mender::common::io;
namespace mlog = mender::common::log;
namespace path = mender::common::path;
namespace procs = mender::common::processes;
namespace mtesting = mender::common::testing;

using TestEventLoop = mender::common::testing::TestEventLoop;

const string TEST_PORT = "8088";

class AuthTests : public testing::Test {
protected:
	mtesting::TemporaryDirectory tmpdir;
	const string test_device_identity_script = path::Join(tmpdir.Path(), "mender-device-identity");

	void SetUp() override {
		// Create the device-identity script
		string script = R"(#!/bin/sh
echo "key1=value1"
echo "key2=value2"
echo "key3=value3"
echo "key1=value11"
exit 0
)";

		ofstream os(test_device_identity_script);
		os << script;
		os.close();

		int ret = chmod(test_device_identity_script.c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
		ASSERT_EQ(ret, 0);
	}
};

class AuthDBusTests : public testing::Test {
protected:
	// Have to use static setup/teardown/data because libdbus doesn't seem to
	// respect changing value of DBUS_SYSTEM_BUS_ADDRESS environment variable
	// and keeps connecting to the first address specified.
	static void SetUpTestSuite() {
		// avoid debug noise from process handling
		mlog::SetLevel(mlog::LogLevel::Warning);

		string dbus_sock_path = "unix:path=" + tmp_dir_.Path() + "/dbus.sock";
		dbus_daemon_proc_.reset(
			new procs::Process {{"dbus-daemon", "--session", "--address", dbus_sock_path}});
		dbus_daemon_proc_->Start();
		// give the DBus daemon time to start and initialize
		std::this_thread::sleep_for(chrono::seconds {1});

		// TIP: Uncomment the code below (and dbus_monitor_proc_
		//      declaration+definition and termination further below) to see
		//      what's going on in the DBus world.
		// dbus_monitor_proc_.reset(
		// 	new procs::Process {{"dbus-monitor", "--address", dbus_sock_path}});
		// dbus_monitor_proc_->Start();
		// // give the DBus monitor time to start and initialize
		// std::this_thread::sleep_for(chrono::seconds {1});

		setenv("DBUS_SYSTEM_BUS_ADDRESS", dbus_sock_path.c_str(), 1);
	};

	static void TearDownTestSuite() {
		dbus_daemon_proc_->EnsureTerminated();
		// dbus_monitor_proc_->EnsureTerminated();
		unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
	};

	void SetUp() override {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
		GTEST_SKIP() << "Thread sanitizer doesn't like what libdbus is doing with locks";
#endif
#endif
	}
	static mtesting::TemporaryDirectory tmp_dir_;
	static unique_ptr<procs::Process> dbus_daemon_proc_;
	// static unique_ptr<procs::Process> dbus_monitor_proc_;
};
mtesting::TemporaryDirectory AuthDBusTests::tmp_dir_;
unique_ptr<procs::Process> AuthDBusTests::dbus_daemon_proc_;
// unique_ptr<procs::Process> AuthDBusTests::dbus_monitor_proc_;

TEST_F(AuthTests, FetchJWTTokenTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";

	TestEventLoop loop;

	// Setup a test server
	const string server_url {"http://127.0.0.1:" + TEST_PORT};
	http::ServerConfig server_config;
	http::Server server(server_config, loop);
	server.AsyncServeUrl(
		server_url,
		[](http::ExpectedIncomingRequestPtr exp_req) {
			ASSERT_TRUE(exp_req) << exp_req.error().String();
			exp_req.value()->SetBodyWriter(make_shared<io::Discard>());
		},
		[JWT_TOKEN](http::ExpectedIncomingRequestPtr exp_req) {
			ASSERT_TRUE(exp_req) << exp_req.error().String();

			auto result = exp_req.value()->MakeResponse();
			ASSERT_TRUE(result);
			auto resp = result.value();

			resp->SetStatusCodeAndMessage(200, "OK");
			resp->SetBodyReader(make_shared<io::StringReader>(JWT_TOKEN));
			resp->SetHeader("Content-Length", to_string(JWT_TOKEN.size()));
			resp->AsyncReply([](error::Error err) { ASSERT_EQ(error::NoError, err); });
		});

	string private_key_path = "./private_key.pem";

	auth::APIResponseHandler handle_jwt_token_callback = [&loop,
														  JWT_TOKEN](auth::APIResponse resp) {
		ASSERT_TRUE(resp);
		EXPECT_EQ(resp.value(), JWT_TOKEN);
		loop.Stop();
	};


	string server_certificate_path {};
	http::ClientConfig client_config {server_certificate_path};
	http::Client client {client_config, loop};

	auto err = auth::FetchJWTToken(
		client,
		server_url,
		private_key_path,
		test_device_identity_script,
		handle_jwt_token_callback);

	loop.Run();

	ASSERT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
}

TEST_F(AuthDBusTests, AuthenticatorBasicTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth simply returning auth data
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"GetJwtToken",
		[JWT_TOKEN, SERVER_URL]() {
			return dbus::StringPair {JWT_TOKEN, SERVER_URL};
		});
	dbus_server.AdvertiseObject(dbus_obj);

	auth::Authenticator authenticator {loop};

	bool action_called = false;
	auto err = authenticator.WithToken(
		[JWT_TOKEN, SERVER_URL, &action_called, &loop](auth::ExpectedAuthData ex_auth_data) {
			action_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);
			loop.Stop();
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;

	loop.Run();
	EXPECT_TRUE(action_called);
}

TEST_F(AuthDBusTests, AuthenticatorTwoActionsTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth simply returning auth data
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"GetJwtToken",
		[JWT_TOKEN, SERVER_URL]() {
			return dbus::StringPair {JWT_TOKEN, SERVER_URL};
		});
	dbus_server.AdvertiseObject(dbus_obj);

	auth::Authenticator authenticator {loop};

	bool action1_called = false;
	bool action2_called = false;
	auto err =
		authenticator.WithToken([JWT_TOKEN, SERVER_URL, &action1_called, &action2_called, &loop](
									auth::ExpectedAuthData ex_auth_data) {
			action1_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);
			if (action1_called && action2_called) {
				loop.Stop();
			}
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;

	err = authenticator.WithToken([JWT_TOKEN, SERVER_URL, &action1_called, &action2_called, &loop](
									  auth::ExpectedAuthData ex_auth_data) {
		action2_called = true;
		ASSERT_TRUE(ex_auth_data);

		EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
		EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);
		if (action1_called && action2_called) {
			loop.Stop();
		}
	});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;

	loop.Run();
	EXPECT_TRUE(action1_called);
	EXPECT_TRUE(action2_called);
}

TEST_F(AuthDBusTests, AuthenticatorTwoActionsWithTokenClearTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth simply returning auth data
	int n_replies = 0;
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"GetJwtToken",
		[JWT_TOKEN, SERVER_URL, &n_replies]() {
			n_replies++;
			return dbus::StringPair {JWT_TOKEN, SERVER_URL};
		});
	dbus_obj->AddMethodHandler<expected::ExpectedBool>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"FetchJwtToken",
		[&n_replies, &dbus_server, JWT_TOKEN, SERVER_URL]() {
			n_replies++;
			dbus_server.EmitSignal<dbus::StringPair>(
				"/io/mender/AuthenticationManager",
				"io.mender.Authentication1",
				"JwtTokenStateChange",
				dbus::StringPair {JWT_TOKEN + "2", SERVER_URL + "2"});

			return true;
		});
	dbus_server.AdvertiseObject(dbus_obj);

	auth::Authenticator authenticator {loop, chrono::seconds {2}};

	bool action1_called = false;
	bool action2_called = false;
	auto err = authenticator.WithToken(
		[JWT_TOKEN, SERVER_URL, &action1_called, &action2_called, &loop, &authenticator](
			auth::ExpectedAuthData ex_auth_data) {
			action1_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);

			authenticator.ExpireToken();

			auto err = authenticator.WithToken([JWT_TOKEN, SERVER_URL, &action2_called, &loop](
												   auth::ExpectedAuthData ex_auth_data) {
				action2_called = true;
				ASSERT_TRUE(ex_auth_data);

				EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN + "2");
				EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL + "2");

				loop.Stop();
			});
			EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
	loop.Run();

	EXPECT_EQ(n_replies, 2);
	EXPECT_TRUE(action1_called);
	EXPECT_TRUE(action2_called);
}

TEST_F(AuthDBusTests, AuthenticatorTwoActionsWithTokenClearAndTimeoutTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth simply returning auth data, but never announcing a
	// new token with a signal
	int n_replies = 0;
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"GetJwtToken",
		[JWT_TOKEN, SERVER_URL, &n_replies]() {
			n_replies++;
			return dbus::StringPair {JWT_TOKEN, SERVER_URL};
		});
	dbus_obj->AddMethodHandler<expected::ExpectedBool>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"FetchJwtToken",
		[&n_replies]() {
			n_replies++;
			// no JwtTokenStateChange signal emitted here
			return true;
		});
	dbus_server.AdvertiseObject(dbus_obj);

	auth::Authenticator authenticator {loop, chrono::seconds {2}};

	bool action1_called = false;
	bool action2_called = false;
	auto err = authenticator.WithToken(
		[JWT_TOKEN, SERVER_URL, &action1_called, &action2_called, &loop, &authenticator](
			auth::ExpectedAuthData ex_auth_data) {
			action1_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);

			authenticator.ExpireToken();

			auto err = authenticator.WithToken([JWT_TOKEN, SERVER_URL, &action2_called, &loop](
												   auth::ExpectedAuthData ex_auth_data) {
				action2_called = true;
				ASSERT_FALSE(ex_auth_data);

				loop.Stop();
			});
			EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
	loop.Run();

	EXPECT_EQ(n_replies, 2);
	EXPECT_TRUE(action1_called);
	EXPECT_TRUE(action2_called);
}

TEST_F(AuthDBusTests, AuthenticatorBasicRealLifeTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth first returning empty data
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager", "io.mender.Authentication1", "GetJwtToken", []() {
			// no token initially
			return dbus::StringPair {"", ""};
		});
	dbus_obj->AddMethodHandler<expected::ExpectedBool>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"FetchJwtToken",
		[&dbus_server, JWT_TOKEN, SERVER_URL]() {
			dbus_server.EmitSignal<dbus::StringPair>(
				"/io/mender/AuthenticationManager",
				"io.mender.Authentication1",
				"JwtTokenStateChange",
				dbus::StringPair {JWT_TOKEN, SERVER_URL});

			return true;
		});
	dbus_server.AdvertiseObject(dbus_obj);

	auth::Authenticator authenticator {loop, chrono::seconds {2}};

	bool action_called = false;
	auto err = authenticator.WithToken(
		[JWT_TOKEN, SERVER_URL, &action_called, &loop](auth::ExpectedAuthData ex_auth_data) {
			action_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);
			loop.Stop();
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;

	loop.Run();
	EXPECT_TRUE(action_called);
}

TEST_F(AuthDBusTests, AuthenticatorExternalTokenUpdateTest) {
	const string JWT_TOKEN = "FOOBARJWTTOKEN";
	const string SERVER_URL = "some.server";

	TestEventLoop loop;

	// Setup fake mender-auth returning auth data
	int n_replies = 0;
	dbus::DBusServer dbus_server {loop, "io.mender.AuthenticationManager"};
	auto dbus_obj = make_shared<dbus::DBusObject>("/io/mender/AuthenticationManager");
	dbus_obj->AddMethodHandler<dbus::ExpectedStringPair>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"GetJwtToken",
		[JWT_TOKEN, SERVER_URL, &n_replies]() {
			n_replies++;
			return dbus::StringPair {JWT_TOKEN, SERVER_URL};
		});
	dbus_obj->AddMethodHandler<expected::ExpectedBool>(
		"io.mender.AuthenticationManager",
		"io.mender.Authentication1",
		"FetchJwtToken",
		[&dbus_server, JWT_TOKEN, SERVER_URL]() {
			dbus_server.EmitSignal<dbus::StringPair>(
				"/io/mender/AuthenticationManager",
				"io.mender.Authentication1",
				"JwtTokenStateChange",
				dbus::StringPair {JWT_TOKEN + "2", SERVER_URL + "2"});

			return true;
		});
	dbus_server.AdvertiseObject(dbus_obj);

	dbus::DBusClient dbus_client {loop};
	auth::Authenticator authenticator {loop, chrono::seconds {2}};

	events::Timer ext_token_fetch_timer {loop};
	events::Timer second_with_token_timer {loop};
	bool action1_called = false;
	bool action2_called = false;
	auto err = authenticator.WithToken(
		[JWT_TOKEN, SERVER_URL, &action1_called](auth::ExpectedAuthData ex_auth_data) {
			action1_called = true;
			ASSERT_TRUE(ex_auth_data);

			EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN);
			EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL);
		});
	EXPECT_EQ(err, error::NoError) << "Unexpected error: " << err.message;
	ext_token_fetch_timer.AsyncWait(chrono::seconds {1}, [&dbus_client](error::Error err) {
		dbus_client.CallMethod<expected::ExpectedBool>(
			"io.mender.AuthenticationManager",
			"/io/mender/AuthenticationManager",
			"io.mender.Authentication1",
			"FetchJwtToken",
			[](expected::ExpectedBool ex_value) {
				ASSERT_TRUE(ex_value);
				ASSERT_TRUE(ex_value.value());
			});
	});
	second_with_token_timer.AsyncWait(
		chrono::seconds {2},
		[JWT_TOKEN, SERVER_URL, &authenticator, &action2_called, &loop](error::Error err) {
			auto lerr = authenticator.WithToken([JWT_TOKEN, SERVER_URL, &action2_called, &loop](
													auth::ExpectedAuthData ex_auth_data) {
				action2_called = true;
				ASSERT_TRUE(ex_auth_data);

				EXPECT_EQ(ex_auth_data.value().token, JWT_TOKEN + "2");
				EXPECT_EQ(ex_auth_data.value().server_url, SERVER_URL + "2");

				loop.Stop();
			});
			EXPECT_EQ(lerr, error::NoError) << "Unexpected error: " << lerr.message;
		});
	loop.Run();
	EXPECT_TRUE(action1_called);
	EXPECT_TRUE(action2_called);

	// GetJwtToken() should have only been called once, by the first
	// WithToken(), the second WithToken() should use the token delivered by the
	// DBus signal.
	EXPECT_EQ(n_replies, 1);
}
