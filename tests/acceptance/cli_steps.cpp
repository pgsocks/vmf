#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace
{

struct cli_context {
	int result = -1;
};

static int run_driver(const char *ruby, const char *script,
	const char *executable, const std::string &scenario)
{
#ifdef _WIN32
	std::string quoted_ruby = "\"" + std::string(ruby) + "\"";
	std::string quoted_script = "\"" + std::string(script) + "\"";
	std::string quoted_executable = "\"" + std::string(executable) + "\"";
	const char *arguments[] = {quoted_ruby.c_str(), quoted_script.c_str(),
		quoted_executable.c_str(), scenario.c_str(), nullptr};

	return static_cast<int>(_spawnv(_P_WAIT, ruby, arguments));
#else
	char *arguments[] = {const_cast<char *>(ruby),
		const_cast<char *>(script), const_cast<char *>(executable),
		const_cast<char *>(scenario.c_str()), nullptr};
	pid_t process;
	int error = posix_spawn(&process, ruby, nullptr, nullptr,
		arguments, environ);

	if (error) {
		errno = error;
		std::perror("CLI test helper");
		return -1;
	}
	int status;

	while (waitpid(process, &status, 0) == -1) {
		if (errno == EINTR)
			continue;
		std::perror("CLI test helper wait");
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

WHEN("^I exercise CLI ([a-z0-9-]+)$")
{
	REGEX_PARAM(std::string, scenario);
	cucumber::ScenarioScope<cli_context> context;
	const char *ruby = std::getenv("VMF_CLI_RUBY");
	const char *script = std::getenv("VMF_CLI_DRIVER");
	const char *executable = std::getenv("VMF_CLI_EXECUTABLE");

	ASSERT_NE(nullptr, ruby);
	ASSERT_NE(nullptr, script);
	ASSERT_NE(nullptr, executable);
	context->result = run_driver(ruby, script, executable, scenario);
}

THEN("^the CLI outcomes match the requested workflow$")
{
	cucumber::ScenarioScope<cli_context> context;

	ASSERT_EQ(0, context->result);
}

}
