#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <debugging/win_x64_gdb_stub_handler.hpp>

//#define CONCISE_EMULATOR_OUTPUT

#include "object_watching.hpp"

bool use_gdb = false;

namespace
{
	void watch_system_objects(windows_emulator& win_emu)
	{
		watch_object(win_emu, *win_emu.current_thread().teb);
		watch_object(win_emu, win_emu.process().peb);
		watch_object(win_emu, emulator_object<KUSER_SHARED_DATA>{win_emu.emu(), kusd_mmio::address()});
		auto* params_hook = watch_object(win_emu, win_emu.process().process_params);

		win_emu.emu().hook_memory_write(win_emu.process().peb.value() + offsetof(PEB, ProcessParameters), 0x8,
		                                [&](const uint64_t address, size_t, const uint64_t value)
		                                {
			                                const auto target_address = win_emu.process().peb.value() + offsetof(
				                                PEB, ProcessParameters);

			                                if (address == target_address)
			                                {
				                                const emulator_object<RTL_USER_PROCESS_PARAMETERS> obj{
					                                win_emu.emu(), value
				                                };

				                                win_emu.emu().delete_hook(params_hook);
				                                params_hook = watch_object(win_emu, obj);
			                                }
		                                });
	}

	void run_emulation(windows_emulator& win_emu)
	{
		try
		{
			if (use_gdb)
			{
				const auto* address = "127.0.0.1:28960";
				win_emu.logger.print(color::pink, "Waiting for GDB connection on %s...\n", address);

				win_x64_gdb_stub_handler handler{win_emu};
				run_gdb_stub(handler, "i386:x86-64", gdb_registers.size(), address);
			}
			else
			{
				win_emu.start();
			}
		}
		catch (const std::exception& e)
		{
			win_emu.logger.print(color::red, "Emulation failed at: 0x%llX - %s\n",
			                     win_emu.emu().read_instruction_pointer(), e.what());
			throw;
		}
		catch (...)
		{
			win_emu.logger.print(color::red, "Emulation failed at: 0x%llX\n", win_emu.emu().read_instruction_pointer());
			throw;
		}

		const auto exit_status = win_emu.process().exit_status;
		if (exit_status.has_value())
		{
			win_emu.logger.print(color::red, "Emulation terminated with status: %X\n", *exit_status);
		}
		else
		{
			win_emu.logger.print(color::red, "Emulation terminated without status!\n");
		}
	}

	std::vector<std::wstring> parse_arguments(char* argv[], const size_t argc)
	{
		std::vector<std::wstring> args{};
		args.reserve(argc - 1);

		for (size_t i = 1; i < argc; ++i)
		{
			std::string_view arg(argv[i]);
			args.emplace_back(arg.begin(), arg.end());
		}

		return args;
	}

	void run(char* argv[], const size_t argc)
	{
		if (argc < 1)
		{
			return;
		}

		emulator_settings settings{
			.application = argv[0],
			.arguments = parse_arguments(argv, argc),
#ifdef CONCISE_EMULATOR_OUTPUT
			.silent_until_main = true,
#endif
		};

		windows_emulator win_emu{std::move(settings)};

		(void)&watch_system_objects;
		watch_system_objects(win_emu);
		win_emu.buffer_stdout = true;
		//win_emu.verbose_calls = true;

		const auto& exe = *win_emu.process().executable;

		for (const auto& section : exe.sections)
		{
			if ((section.region.permissions & memory_permission::exec) != memory_permission::exec)
			{
				continue;
			}

			auto read_handler = [&, section](const uint64_t address, size_t, uint64_t)
			{
				const auto rip = win_emu.emu().read_instruction_pointer();
				if (rip >= section.region.start && rip < section.region.start + section.
					region.length)
				{
#ifdef CONCISE_EMULATOR_OUTPUT
					static uint64_t count{0};
					++count;
					if (count > 100 && count % 10000 != 0) return;
#endif

					win_emu.logger.print(
						color::green,
						"Reading from executable section %s: 0x%llX at 0x%llX\n",
						section.name.c_str(), address, rip);
				}
			};

			const auto write_handler = [&, section](const uint64_t address, size_t, uint64_t)
			{
				const auto rip = win_emu.emu().read_instruction_pointer();
				if (rip >= section.region.start && rip < section.region.start + section.
					region.length)
				{
#ifdef CONCISE_EMULATOR_OUTPUT
					static uint64_t count{0};
					++count;
					if (count > 100 && count % 10000 != 0) return;
#endif

					win_emu.logger.print(
						color::cyan,
						"Writing to executable section %s: 0x%llX at 0x%llX\n",
						section.name.c_str(), address, rip);
				}
			};

			win_emu.emu().hook_memory_read(section.region.start, section.region.length, std::move(read_handler));
			win_emu.emu().hook_memory_write(section.region.start, section.region.length, std::move(write_handler));
		}

		run_emulation(win_emu);
	}
}

int main(const int argc, char** argv)
{
	if (argc <= 1)
	{
		puts("Application not specified!");
		return 1;
	}

	//setvbuf(stdout, nullptr, _IOFBF, 0x10000);
	if (argc > 2 && argv[1] == "-d"sv)
	{
		use_gdb = true;
	}

	try
	{
		do
		{
			const auto offset = use_gdb ? 2 : 1;
			run(argv + offset, static_cast<size_t>(argc - offset));
		}
		while (use_gdb);

		return 0;
	}
	catch (std::exception& e)
	{
		puts(e.what());

#if defined(_WIN32) && 0
		MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
	}

	return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
	return main(__argc, __argv);
}
#endif
