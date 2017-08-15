/*
  Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

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
#include "router_component_test.h"

#include <fstream>
#include <functional>

using testing::HasSubstr;
Path g_origin_path;

class RouterLoggingTest : public RouterComponentTest, public ::testing::Test {
 protected:
  virtual void SetUp() {
    set_origin(g_origin_path);
    RouterComponentTest::SetUp();
  }

  bool find_in_log(const std::string logging_folder, const std::function<bool(const std::string&)>& predicate) {
    // This is proxy function to account for the fact that I/O can sometimes be slow.
    // If real_find_in_log() fails, it will retry 3 more times

    bool res = false;
    for (int retries_left = 3; retries_left; retries_left--) {
      try {
        res = real_find_in_log(logging_folder, predicate);
      } catch (const std::runtime_error&) {
        // report I/O error only on the last attempt
        if (retries_left == 1) {
          std::cerr << "  find_in_log() failed, giving up." << std::endl;
          throw;
        }
      }

      if (res)
        return true;
      if (retries_left) {
        std::cerr << "  find_in_log() failed, sleeping a bit and retrying..." << std::endl;
#ifdef _WIN32
        Sleep(5000);
#else
        sleep(5);
#endif
      }
    }

    return false;
  }

 private:
  bool real_find_in_log(const std::string logging_folder, const std::function<bool(const std::string&)>& predicate) {
    Path file(logging_folder + "/" + "mysqlrouter.log");
    std::ifstream ifs(file.c_str());
    if (!ifs)
      throw std::runtime_error("Error opening file " + file.str());

    std::string line;
    while (std::getline(ifs, line))
      if (predicate(line))
        return true;
    return false;
  }

};

TEST_F(RouterLoggingTest, log_startup_failure_to_console) {
  // This test verifies that fatal error message thrown in MySQLRouter::start()
  // during startup (before Loader::start() takes over) are properly logged to STDERR

  std::string conf_file = create_config_file("");

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear on STDERR
  // 2017-06-18 15:24:32 main ERROR [7ffff7fd4780] Error: MySQL Router not configured to load or start any plugin. Exiting.
  std::string out = router.get_full_output();
  EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
  EXPECT_THAT(out.c_str(), HasSubstr(" Error: MySQL Router not configured to load or start any plugin. Exiting."));
}

TEST_F(RouterLoggingTest, log_startup_failure_to_logfile) {
  // This test is the same as log_startup_failure_to_logfile(), but the failure
  // message is expected to be logged into a logfile

  // create tmp dir where we will log
  const std::string logging_folder = get_tmp_dir();
  std::shared_ptr<void> exit_guard(nullptr, [&](void*){purge_dir(logging_folder);});

  // create config with logging_folder set to that directory
  std::map<std::string, std::string> params = get_DEFAULT_defaults();
  params.at("logging_folder") = logging_folder;
  std::string conf_file = create_config_file("", &params);

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear in log:
  // 2017-06-18 15:24:32 main ERROR [7ffff7fd4780] Error: MySQL Router not configured to load or start any plugin. Exiting.
  auto matcher = [](const std::string& line) -> bool {
    return line.find(" main ERROR ") != line.npos &&
           line.find(" Error: MySQL Router not configured to load or start any plugin. Exiting.") != line.npos;
  };
  EXPECT_TRUE(find_in_log(logging_folder, matcher));
}

TEST_F(RouterLoggingTest, bad_logging_folder) {
  // This test verifies that invalid logging_folder is properly handled and
  // appropriate message is printed on STDERR. Router tries to mkdir(logging_folder)
  // if it doesn't exist, then write its log inside of it.

  // create tmp dir to contain our tests
  const std::string tmp_dir = get_tmp_dir();
  std::shared_ptr<void> exit_guard(nullptr, [&](void*){purge_dir(tmp_dir);});

// unfortunately it's not (reasonably) possible to make folders read-only on Windows,
// therefore we can run the following 2 tests only on Unix
// https://support.microsoft.com/en-us/help/326549/you-cannot-view-or-change-the-read-only-or-the-system-attributes-of-fo
#ifndef _WIN32

  // make tmp dir read-only
  chmod(tmp_dir.c_str(), S_IRUSR | S_IXUSR); // r-x for the user (aka 500)

  // logging_folder doesn't exist and can't be created
  {
    std::string logging_dir = tmp_dir + "/some_dir";

    // create Router config
    std::map<std::string, std::string> params = get_DEFAULT_defaults();
    params.at("logging_folder") = logging_dir;
    std::string conf_file = create_config_file("", &params);

    // run the router and wait for it to exit
    auto router = launch_router("-c " +  conf_file);
    EXPECT_EQ(router.wait_for_exit(), 1);

    // expect something like this to appear on STDERR
    // 2017-07-28 12:24:11 main ERROR [7f6eafe41780] Error: Error when creating dir '/bla': 13
    std::string out = router.get_full_output();
    EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
    EXPECT_THAT(out.c_str(), HasSubstr(" Error: Error when creating dir '" + logging_dir + "': 13"));
  }

  // logging_folder exists but is not writeable
  {
    std::string logging_dir = tmp_dir;

    // create Router config
    std::map<std::string, std::string> params = get_DEFAULT_defaults();
    params.at("logging_folder") = logging_dir;
    std::string conf_file = create_config_file("", &params);

    // run the router and wait for it to exit
    auto router = launch_router("-c " +  conf_file);
    EXPECT_EQ(router.wait_for_exit(), 1);

    // expect something like this to appear on STDERR
    // 2017-07-28 15:48:40 main ERROR [7f41b826a780] Error: Failed to open //mysqlrouter.log: Permission denied
    std::string out = router.get_full_output();
    EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
    EXPECT_THAT(out.c_str(), HasSubstr(" Error: Failed to open " + logging_dir + "/mysqlrouter.log: Permission denied"));
  }

  // restore writability to tmp dir
  chmod(tmp_dir.c_str(), S_IRUSR | S_IWUSR | S_IXUSR); // rwx for the user (aka 700)

#endif // #ifndef _WIN32

  // logging_folder is really a file
  {
    std::string logging_dir = tmp_dir + "/some_file";

    // create that file
    {
      std::ofstream some_file(logging_dir);
      EXPECT_TRUE(some_file.good());
    }

    // create Router config
    std::map<std::string, std::string> params = get_DEFAULT_defaults();
    params.at("logging_folder") = logging_dir;
    std::string conf_file = create_config_file("", &params);

    // run the router and wait for it to exit
    auto router = launch_router("-c " +  conf_file);
    EXPECT_EQ(router.wait_for_exit(), 1);

    // expect something like this to appear on STDERR
    // 2017-07-28 15:52:43 main ERROR [7fcbbfb90780] Error: Failed to open /etc/passwd/mysqlrouter.log: Not a directory
    std::string out = router.get_full_output();
    EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
#ifndef _WIN32
    EXPECT_THAT(out.c_str(), HasSubstr(" Error: Failed to open " + logging_dir + "/mysqlrouter.log: Not a directory"));
#else
    EXPECT_THAT(out.c_str(), HasSubstr(" Error: Failed to open " + logging_dir + "/mysqlrouter.log: No such file or directory"));
#endif
  }
}

TEST_F(RouterLoggingTest, logger_section_with_key) {
  // This test verifies that [logger:with_some_key] section is handled properly
  // Router should report the error on STDERR and exit

  std::string conf_file = create_config_file("[logger:some_key]\n");

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear on STDERR
  // 2017-08-02 16:44:39 main ERROR [7ff0cf635780] Error: Section 'logger' does not support keys
  std::string out = router.get_full_output();
  EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
  EXPECT_THAT(out.c_str(), HasSubstr(" Error: Section 'logger' does not support keys"));
}

TEST_F(RouterLoggingTest, multiple_logger_sections) {
  // This test verifies that multiple [logger] sections are handled properly.
  // Router should report the error on STDERR and exit

  std::string conf_file = create_config_file("[logger]\n[logger]\n");

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear on STDERR
  // 2017-08-02 16:47:35 main ERROR [7f756674d780] Error: Configuration error: Section 'logger' given more than once. Please use keys to give multiple sections. For example 'logger:one' and 'logger:two' to give two sections for plugin 'logger'.
  std::string out = router.get_full_output();
  EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
  EXPECT_THAT(out.c_str(), HasSubstr(" Error: Configuration error: Section 'logger' given more than once. Please use keys to give multiple sections. For example 'logger:one' and 'logger:two' to give two sections for plugin 'logger'."));
}

TEST_F(RouterLoggingTest, bad_loglevel) {
  // This test verifies that bad log level in [logger] section is handled properly.
  // Router should report the error on STDERR and exit

  const std::string conf_file = create_config_file("[logger]\nlevel = UNKNOWN\n");

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear on STDERR
  // 2017-08-14 16:03:44 main ERROR [7f7a61be6780] Configuration error: Log level 'unknown' is not valid. Valid values are: debug, error, fatal, info, and warning
  const std::string out = router.get_full_output();
  EXPECT_THAT(out.c_str(), HasSubstr(" main ERROR "));
  EXPECT_THAT(out.c_str(), HasSubstr(" Configuration error: Log level 'unknown' is not valid. Valid values are: debug, error, fatal, info, and warning"));
}

TEST_F(RouterLoggingTest, bad_loglevel_gets_logged) {
  // This test is the same as bad_loglevel(), but the failure
  // message is expected to be logged into a logfile

  // create tmp dir where we will log
  const std::string logging_folder = get_tmp_dir();
  std::shared_ptr<void> exit_guard(nullptr, [&](void*){purge_dir(logging_folder);});

  // create config with logging_folder set to that directory
  std::map<std::string, std::string> params = get_DEFAULT_defaults();
  params.at("logging_folder") = logging_folder;
  const std::string conf_file = create_config_file("[logger]\nlevel = UNKNOWN\n", &params);

  // run the router and wait for it to exit
  auto router = launch_router("-c " +  conf_file);
  EXPECT_EQ(router.wait_for_exit(), 1);

  // expect something like this to appear on STDERR
  // 2017-08-14 16:03:44 main ERROR [7f7a61be6780] Configuration error: Log level 'unknown' is not valid. Valid values are: debug, error, fatal, info, and warning
  auto matcher = [](const std::string& line) -> bool {
    return line.find(" main ERROR ") != line.npos &&
           line.find(" Configuration error: Log level 'unknown' is not valid. Valid values are: debug, error, fatal, info, and warning") != line.npos;
  };
  EXPECT_TRUE(find_in_log(logging_folder, matcher));
}

int main(int argc, char *argv[]) {
  g_origin_path = Path(argv[0]).dirname();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}