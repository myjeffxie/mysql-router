/*
  Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "gmock/gmock.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#include <sys/un.h>
#endif

#include "common.h"

#include "mysql/harness/config_parser.h"
#include "mysql/harness/filesystem.h"
#include "mysql/harness/plugin.h"

#include "cmd_exec.h"
#include "gtest_consoleoutput.h"
#include "logger.h"
#include "mysql_routing.h"
#include "plugin_config.h"
#include "router_test_helpers.h"

// since this function is only meant to be used here (for testing purposes), it's not available in the headers.
void validate_socket_info_test_proxy(const std::string& err_prefix, const mysql_harness::ConfigSection* section, const RoutingPluginConfig& config);

using std::string;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::NotNull;
using ::testing::StrEq;
using mysql_harness::Path;
using mysql_harness::get_strerror;

// define what is available in routing_plugin.cc
extern mysql_harness::Plugin harness_plugin_routing;
extern const mysql_harness::AppInfo *g_app_info;
extern const char *kRoutingRequires[1];

int init(const mysql_harness::AppInfo *info);

void start(const mysql_harness::ConfigSection *section);

string g_cwd;
Path g_origin;

class RoutingPluginTests : public ConsoleOutputTest {
protected:
  virtual void SetUp() {
    set_origin(g_origin);
    ConsoleOutputTest::SetUp();
    config_path.reset(new Path(g_cwd));
    config_path->append("test_routing_plugin.ini");
    cmd = app_mysqlrouter->str() + " -c " + config_path->str();

    bind_address = "127.0.0.1:15508";
    destinations = "127.0.0.1:3306";
    socket = rundir + "/unix_socket";
    mode = "read-only";
    connect_timeout = "1";
    client_connect_timeout = "9";
    max_connect_errors = "100";
    protocol = "classic";
  }

  bool in_missing(std::vector<std::string> missing, std::string needle) {
    return std::find(missing.begin(), missing.end(), needle) != missing.end();
  }

  void reset_config(std::vector<std::string> missing = {}, bool add_break = false) {
    std::ofstream ofs_config(config_path->str());
    if (ofs_config.good()) {
      ofs_config << "[DEFAULT]\n";
      ofs_config << "logging_folder =\n";
      ofs_config << "plugin_folder = " << plugin_dir->str() << "\n";
      ofs_config << "runtime_folder = " << stage_dir->str() << "\n";
      ofs_config << "config_folder = " << stage_dir->str() << "\n";
      ofs_config << "data_folder = " << stage_dir->str() << "\n\n";
      ofs_config << "[routing:tests]\n";

      using ConfigOption = std::pair<std::string, std::string&>;
      std::vector<ConfigOption> routing_config_options{
        {"bind_address",            std::ref(bind_address)},
        {"socket",                  std::ref(socket)},
        {"destinations",            std::ref(destinations)},
        {"mode",                    std::ref(mode)},
        {"connect_timeout",         std::ref(connect_timeout)},
        {"client_connect_timeout",  std::ref(client_connect_timeout)},
        {"max_connect_errors",      std::ref(max_connect_errors)},
        {"protocol",                std::ref(protocol)}
      };
      for (auto& option: routing_config_options) {
        if (!in_missing(missing, option.first)) {
          ofs_config << option.first  << " = " << option.second << "\n";
        }
      }

      // Following is an incorrect [routing] entry. If the above is valid, this
      // will make sure Router stops.
      if (add_break) {
        ofs_config << "\n[routing:break]\n";
      }

      ofs_config << "\n";
      ofs_config.close();
    }
  }

  virtual void TearDown() {
    if (unlink(config_path->c_str()) == -1) {
      if (errno != ENOENT) {
        // File missing is OK.
        std::cerr << "Failed removing " << config_path->str()
        << ": " << get_strerror(errno) << "(" << errno << ")" << std::endl;
      }
    }
    ConsoleOutputTest::TearDown();
  }

  const string plugindir = "path/to/plugindir";
  const string logdir = "/path/to/logdir";
  const string program = "routing_plugin_test";
  const string rundir = "/path/to/rundir";
  const string cfgdir = "/path/to/cfgdir";
  const string datadir = "/path/to/datadir";
  string bind_address;
  string destinations;
  string socket;
  string mode;
  string connect_timeout;
  string client_connect_timeout;
  string max_connect_errors;
  string protocol;

  std::unique_ptr<Path> config_path;
  std::string cmd;
};

TEST_F(RoutingPluginTests, PluginConstants) {
  // Check number of required plugins
  ASSERT_EQ(1UL, sizeof(kRoutingRequires) / sizeof(*kRoutingRequires));
  // Check the required plugins
  ASSERT_THAT(kRoutingRequires[0], StrEq("logger"));
}

TEST_F(RoutingPluginTests, PluginObject) {
  ASSERT_EQ(harness_plugin_routing.abi_version, 0x0101U);
  ASSERT_EQ(harness_plugin_routing.plugin_version, static_cast<uint32_t>(VERSION_NUMBER(0, 0, 1)));
  ASSERT_EQ(harness_plugin_routing.requires_length, 1U);
  ASSERT_THAT(harness_plugin_routing.requires[0], StrEq("logger"));
  ASSERT_EQ(harness_plugin_routing.conflicts_length, 0U);
  ASSERT_THAT(harness_plugin_routing.conflicts, IsNull());
  ASSERT_THAT(harness_plugin_routing.deinit, IsNull());
  ASSERT_THAT(harness_plugin_routing.brief,
              StrEq("Routing MySQL connections between MySQL clients/connectors and servers"));
}

TEST_F(RoutingPluginTests, InitAppInfo) {
  ASSERT_THAT(g_app_info, IsNull());

  mysql_harness::AppInfo test_app_info{
      program.c_str(),
      plugindir.c_str(),
      logdir.c_str(),
      rundir.c_str(),
      cfgdir.c_str(),
      datadir.c_str(),
      nullptr
  };

  int res = harness_plugin_routing.init(&test_app_info);
  ASSERT_EQ(res, 0);

  ASSERT_THAT(g_app_info, Not(IsNull()));
  ASSERT_THAT(program.c_str(), StrEq(g_app_info->program));
}

TEST_F(RoutingPluginTests, StartCorrectSection) {
  reset_config({}, true);
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output, HasSubstr("[routing:break]"));
}

TEST_F(RoutingPluginTests, StartMissingMode) {
  reset_config({"mode"});
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("option mode in [routing:tests] needs to be specified; valid are"));
}

TEST_F(RoutingPluginTests, StartCaseInsensitiveMode) {
  mode = "Read-Only";
  reset_config({}, true);
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              Not(HasSubstr("valid are")));
}

TEST_F(RoutingPluginTests, NoListeningSocket) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");

  try {
    RoutingPluginConfig config(&section);
    FAIL() << "Expected std::invalid_argument to be thrown";
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("either bind_address or socket option needs to be supplied, or both", e.what());
    SUCCEED();
  } catch (...) {
    FAIL() << "Expected std::invalid_argument to be thrown";
  }
}

TEST_F(RoutingPluginTests, ListeningTcpSocket) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("bind_address", "127.0.0.1:15508");

  EXPECT_NO_THROW({
    RoutingPluginConfig config(&section);
    validate_socket_info_test_proxy("", &section, config);
  });

}

#ifndef _WIN32
TEST_F(RoutingPluginTests, ListeningUnixSocket) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("socket", "./socket");

  EXPECT_NO_THROW({
    RoutingPluginConfig config(&section);
    validate_socket_info_test_proxy("", &section, config);
  });
}

TEST_F(RoutingPluginTests, ListeningBothSockets) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("bind_address", "127.0.0.1:15508");
  section.add("socket", "./socket");

  EXPECT_NO_THROW({
    RoutingPluginConfig config(&section);
    validate_socket_info_test_proxy("", &section, config);
  });
}

TEST_F(RoutingPluginTests, TwoUnixSocketsWithoutTcp) {
  mysql_harness::Config cfg;
  mysql_harness::ConfigSection& section1 = cfg.add("routing", "test_route1");
  section1.add("destinations", "localhost:1234");
  section1.add("mode", "read-only");
  section1.add("socket", "./socket1");
  mysql_harness::ConfigSection& section2 = cfg.add("routing", "test_route2");
  section2.add("destinations", "localhost:1234");
  section2.add("mode", "read-only");
  section2.add("socket", "./socket2");

  EXPECT_NO_THROW({
    mysql_harness::AppInfo info;
    info.config = &cfg;
    harness_plugin_routing.init(&info);
  });
}

TEST_F(RoutingPluginTests, TwoUnixSocketsWithTcp) {
  mysql_harness::Config cfg;
  mysql_harness::ConfigSection& section1 = cfg.add("routing", "test_route1");
  section1.add("destinations", "localhost:1234");
  section1.add("mode", "read-only");
  section1.add("bind_address", "127.0.0.1:15501");
  section1.add("socket", "./socket1");
  mysql_harness::ConfigSection& section2 = cfg.add("routing", "test_route2");
  section2.add("destinations", "localhost:1234");
  section2.add("mode", "read-only");
  section2.add("bind_address", "127.0.0.1:15502");
  section2.add("socket", "./socket2");

  EXPECT_NO_THROW({
    mysql_harness::AppInfo info;
    info.config = &cfg;
    harness_plugin_routing.init(&info);
  });
}

static std::string make_string(size_t len, char c = 'a') {
  std::string res(len, c);

  assert(res.length() == len);

  return res;
}

static void test_socket_length(const std::string& socket_name, size_t max_len) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("socket", socket_name);

  if (socket_name.length() <= max_len) {
    EXPECT_NO_THROW({
      RoutingPluginConfig config(&section);
      validate_socket_info_test_proxy("", &section, config);
    });
  }
  else {
    EXPECT_THROW(
      RoutingPluginConfig config(&section),
      std::invalid_argument);
  }
}

TEST_F(RoutingPluginTests, ListeningSocketNameLength) {
#ifndef _WIN32
  const size_t MAX_SOCKET_NAME_LEN = sizeof(sockaddr_un().sun_path)-1;
#else
  // doesn't really matter
  const size_t MAX_SOCKET_NAME_LEN = 100;
#endif

  std::string socket_name;

  socket_name = make_string(MAX_SOCKET_NAME_LEN-1);
  test_socket_length(socket_name, MAX_SOCKET_NAME_LEN);

  socket_name = make_string(MAX_SOCKET_NAME_LEN);
  test_socket_length(socket_name, MAX_SOCKET_NAME_LEN);

  socket_name = make_string(MAX_SOCKET_NAME_LEN+1);
  test_socket_length(socket_name, MAX_SOCKET_NAME_LEN);
}

TEST_F(RoutingPluginTests, TwoNonuniqueUnixSockets) {
  // TODO add after implementing plugin lifecycle (WL9558),
  // use TwoNonuniqueTcpSockets as an example
  // (exception is thrown in plugin start(), in a separate thread)
}

#endif

TEST_F(RoutingPluginTests, TwoNonuniqueTcpSockets) {
  mysql_harness::Config cfg;
  mysql_harness::ConfigSection& section1 = cfg.add("routing", "test_route1");
  section1.add("destinations", "localhost:1234");
  section1.add("mode", "read-only");
  section1.add("bind_address", "127.0.0.1:15508");
  mysql_harness::ConfigSection& section2 = cfg.add("routing", "test_route2");
  section2.add("destinations", "localhost:1234");
  section2.add("mode", "read-only");
  section2.add("bind_address", "127.0.0.1:15508");

  try {
    mysql_harness::AppInfo info;
    info.config = &cfg;
    harness_plugin_routing.init(&info);
    FAIL() << "Expected std::invalid_argument to be thrown";
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("in [routing:test_route2]: duplicate IP or name found in bind_address '127.0.0.1:15508'", e.what());
    SUCCEED();
  } catch (...) {
    FAIL() << "Expected std::invalid_argument to be thrown";
  }
}

TEST_F(RoutingPluginTests, StartMissingDestination) {
  {
    reset_config({"destinations"});
    auto cmd_result = cmd_exec(cmd, true);
    ASSERT_THAT(cmd_result.output,
                HasSubstr("option destinations in [routing:tests] is required"));
  }

  {
    destinations = {};
    reset_config({});
    auto cmd_result = cmd_exec(cmd, true);
    ASSERT_THAT(cmd_result.output,
                HasSubstr("option destinations in [routing:tests] is required and needs a value"));
  }
}

TEST_F(RoutingPluginTests, StartImpossiblePortNumber) {
  bind_address = "127.0.0.1:99999";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("incorrect (invalid TCP port: impossible port number)"));
}

TEST_F(RoutingPluginTests, StartImpossibleIPAddress) {
  bind_address = "512.512.512.512:3306";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("in [routing:tests]: invalid IP or name in bind_address '512.512.512.512:3306'"));
}

#ifndef _WIN32
TEST_F(RoutingPluginTests, EmptyUnixSocket) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("socket", "");

  // If this not provided, RoutingPluginConfig() will throw with its own error, which will be misleading.
  // This line should not influence throwing/not throwing the right error ("invalid socket ''")
  section.add("bind_address", "127.0.0.1:15508");

  try {
    RoutingPluginConfig config(&section);
    validate_socket_info_test_proxy("", &section, config);
    FAIL() << "Expected std::invalid_argument to be thrown";
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("invalid socket ''", e.what());
    SUCCEED();
  } catch (...) {
    FAIL() << "Expected std::invalid_argument to be thrown";
  }
}

TEST_F(RoutingPluginTests, StartBadUnixSocket) {
  socket = "/this/path/does/not/exist/socket";
  reset_config();
  CmdExecResult cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output, HasSubstr("Setting up named socket service '/this/path/does/not/exist/socket': No such file or directory"));
}

TEST_F(RoutingPluginTests, ListeningHostIsInvalid) {
  mysql_harness::Config         cfg;
  mysql_harness::ConfigSection& section = cfg.add("routing", "test_route");
  section.add("destinations", "localhost:1234");
  section.add("mode", "read-only");
  section.add("bind_address", "host.that.does.not.exist:15508");

  try {
    RoutingPluginConfig config(&section);
    validate_socket_info_test_proxy("", &section, config);
    FAIL() << "Expected std::invalid_argument to be thrown";
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("invalid IP or name in bind_address 'host.that.does.not.exist:15508'", e.what());
    SUCCEED();
  } catch (...) {
    FAIL() << "Expected std::invalid_argument to be thrown";
  }
}
#endif

TEST_F(RoutingPluginTests, StartWithBindAddressInDestinations) {
  bind_address = "127.0.0.1:3306";
  destinations = "127.0.0.1";  // default port is 3306
#ifndef _WIN32
  reset_config();
#else
  reset_config({"socket"});
#endif
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output, HasSubstr("Bind Address can not be part of destinations"));
}

TEST_F(RoutingPluginTests, StartConnectTimeoutSetNegative) {
  connect_timeout = "-1";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("connect_timeout in [routing:tests] needs value between 1 and 65535 inclusive, was '-1'"));
}

TEST_F(RoutingPluginTests, StartClientConnectTimeoutSetIncorrectly) {
  {
    client_connect_timeout = "1";
    reset_config();
    auto cmd_result = cmd_exec(cmd, true);
    ASSERT_THAT(cmd_result.output, HasSubstr(
        "option client_connect_timeout in [routing:tests] needs value between 2 and 31536000 inclusive, was '1'"));
  }

  {
    client_connect_timeout = "31536001";  // 31536000 is maximum
    reset_config();
    auto cmd_result = cmd_exec(cmd, true);
    ASSERT_THAT(cmd_result.output, HasSubstr(
        "option client_connect_timeout in [routing:tests] needs "
            "value between 2 and 31536000 inclusive, was '31536001'"));
  }
}

TEST_F(RoutingPluginTests, StartMaxConnectErrorsSetIncorrectly) {
  {
    max_connect_errors = "0";
    reset_config();
    auto cmd_result = cmd_exec(cmd, true);
    ASSERT_THAT(cmd_result.output, HasSubstr(
        "option max_connect_errors in [routing:tests] needs value between 1 and 4294967295 inclusive, was '0'"));
  }
}

TEST_F(RoutingPluginTests, StartTimeoutsSetToZero) {

  connect_timeout = "0";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr(
                  "option connect_timeout in [routing:tests] needs value between 1 and 65535 inclusive, was '0'"));
}

TEST_F(RoutingPluginTests, EmptyProtocolName) {
  protocol = "";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("Configuration error: Invalid protocol name: ''"));
}

TEST_F(RoutingPluginTests, InvalidProtocolName) {
  protocol = "invalid";
  reset_config();
  auto cmd_result = cmd_exec(cmd, true);
  ASSERT_THAT(cmd_result.output,
              HasSubstr("Configuration error: Invalid protocol name: 'invalid'"));
}

int main(int argc, char *argv[]) {
  init_windows_sockets();
  g_origin = Path(argv[0]).dirname();
  g_cwd = Path(argv[0]).dirname().str();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
