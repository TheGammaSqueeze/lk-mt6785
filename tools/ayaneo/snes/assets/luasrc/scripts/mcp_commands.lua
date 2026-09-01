function concat_options(arg_1_0)
	if arg_1_0 then
		return table.concat(arg_1_0, " ")
	end

	return nil
end

local var_0_0 = Application.exit
local var_0_1 = false

function Application.exit(...)
	var_0_1 = true

	return var_0_0(...)
end

mcp = class()

function mcp.waitTransition()
	if WAIT_TRANSITION_FD and WAIT_TRANSITION_FD >= 0 then
		local var_3_0, var_3_1 = Application.eventFdRead(WAIT_TRANSITION_FD)

		print("Application.eventFdRead(WAIT_TRANSITION_FD) => " .. tostring(var_3_0) .. ", " .. tostring(var_3_1))
	end
end

function mcp.startTransition()
	if START_TRANSITION_FD and START_TRANSITION_FD >= 0 then
		local var_4_0 = Application.eventFdWrite(START_TRANSITION_FD, 1)

		print("Application.eventFdWrite(START_TRANSITION_FD, 1) ==> " .. tostring(var_4_0))
	end
end

function mcp.finishTransition()
	local var_5_0 = not var_0_1

	if var_0_1 then
		SoundComponent.closeNativeDevice()
	end

	if FINISH_TRANSITION_FD and FINISH_TRANSITION_FD >= 0 then
		local var_5_1 = Application.eventFdWrite(FINISH_TRANSITION_FD, 1)

		print("Application.eventFdWrite(FINISH_TRANSITION_FD, 1) ==> " .. tostring(var_5_1))
	end

	if var_5_0 and WAIT_TRANSITION_FD and WAIT_TRANSITION_FD >= 0 then
		Application.eventFdWrite(WAIT_TRANSITION_FD, 1)
	end
end

function mcp.make_command(arg_6_0, arg_6_1, arg_6_2)
	return {
		command = arg_6_0,
		title_id = tostring(arg_6_1),
		display_mode = arg_6_2
	}
end

function mcp.write_file_atomic(arg_7_0, arg_7_1)
	local var_7_0 = arg_7_0 .. "_tmp"
	local var_7_1 = io.open(var_7_0, "w")

	if var_7_1 then
		var_7_1:write(arg_7_1)
		var_7_1:flush()
		var_7_1:close()
		os.rename(var_7_0, arg_7_0)
	end
end

function mcp.write_command(arg_8_0)
	local var_8_0 = ""

	for iter_8_0, iter_8_1 in pairs(arg_8_0) do
		if iter_8_1 ~= nil then
			var_8_0 = var_8_0 .. tostring(iter_8_0) .. "=" .. tostring(iter_8_1) .. "\n"
		end
	end

	mcp.write_file_atomic(MCP_SHARED_FILE, var_8_0)
	mcp.startTransition()
end

function mcp.launch_title(arg_9_0, arg_9_1, arg_9_2, arg_9_3, arg_9_4, arg_9_5, arg_9_6, arg_9_7, arg_9_8)
	local var_9_0 = mcp.make_command("start-game", arg_9_0, arg_9_2.mode)

	var_9_0.save_suspendpoint_filename = arg_9_1
	var_9_0.load_time_filename = arg_9_4
	var_9_0.save_time_filename = arg_9_5
	var_9_0.sram_filename = arg_9_3
	var_9_0.screenshot_filename = arg_9_6
	var_9_0.decorative_filename = arg_9_7
	var_9_0.raw_options = concat_options(arg_9_8)

	mcp.write_command(var_9_0)
	Application.exit()
end

function mcp.resume_title(arg_10_0, arg_10_1, arg_10_2, arg_10_3, arg_10_4, arg_10_5, arg_10_6, arg_10_7, arg_10_8, arg_10_9)
	local var_10_0 = mcp.make_command("resume-game", arg_10_0, arg_10_3.mode)

	var_10_0.load_suspendpoint_filename = arg_10_1
	var_10_0.save_suspendpoint_filename = arg_10_2
	var_10_0.load_time_filename = arg_10_5
	var_10_0.save_time_filename = arg_10_6
	var_10_0.sram_filename = arg_10_4
	var_10_0.screenshot_filename = arg_10_7
	var_10_0.decorative_filename = arg_10_8
	var_10_0.raw_options = concat_options(arg_10_9)

	mcp.write_command(var_10_0)
	Application.exit()
end

function mcp.start_super_play(arg_11_0, arg_11_1, arg_11_2, arg_11_3, arg_11_4, arg_11_5)
	local var_11_0 = mcp.make_command("super-play", arg_11_0, arg_11_3.mode)

	var_11_0.load_suspendpoint_filename = arg_11_1
	var_11_0.keyinput_filename = arg_11_2
	var_11_0.decorative_filename = arg_11_4
	var_11_0.raw_options = concat_options(arg_11_5)

	mcp.write_command(var_11_0)
	Application.exit()
end

function mcp.reset_to_factory(arg_12_0)
	local var_12_0 = mcp.make_command("factory-reset")

	mcp.write_command(var_12_0)
	Application.exit()
end

function mcp.set_language(arg_13_0, arg_13_1)
	mcp.write_file_atomic(LANGUAGE_FILE, arg_13_1 .. "_" .. arg_13_0)
end

function mcp.set_autoshutdown_timer(arg_14_0)
	mcp.write_file_atomic("/var/lib/clover/profiles/0/shutdown.txt", tostring(arg_14_0))
end

function mcp.set_dimming_timer(arg_15_0)
	mcp.write_file_atomic("/var/lib/clover/profiles/0/dimming.txt", tostring(arg_15_0))
end

mcp2 = class(mcp)

function mcp2.launch_title(arg_16_0, arg_16_1, arg_16_2, arg_16_3, arg_16_4, arg_16_5, arg_16_6, arg_16_7, arg_16_8)
	local var_16_0 = mcp.make_command("start-game", arg_16_0, arg_16_2.mode)

	var_16_0.rollback_mode = "record"
	var_16_0.save_rollbackdata_path = arg_16_1
	var_16_0.load_time_filename = arg_16_4
	var_16_0.save_time_filename = arg_16_5
	var_16_0.sram_filename = arg_16_3
	var_16_0.screenshot_filename = arg_16_6
	var_16_0.decorative_filename = arg_16_7
	var_16_0.raw_options = concat_options(arg_16_8)

	mcp.write_command(var_16_0)
	Application.exit()
end

function mcp2.resume_title_record(arg_17_0, arg_17_1, arg_17_2, arg_17_3, arg_17_4, arg_17_5, arg_17_6, arg_17_7, arg_17_8, arg_17_9)
	local var_17_0 = mcp.make_command("resume-game", arg_17_0, arg_17_3.mode)

	var_17_0.rollback_mode = "record"
	var_17_0.load_rollbackdata_path = arg_17_1
	var_17_0.save_rollbackdata_path = arg_17_2
	var_17_0.load_time_filename = arg_17_5
	var_17_0.save_time_filename = arg_17_6
	var_17_0.sram_filename = arg_17_4
	var_17_0.screenshot_filename = arg_17_7
	var_17_0.decorative_filename = arg_17_8
	var_17_0.raw_options = concat_options(arg_17_9)

	mcp.write_command(var_17_0)
end

function mcp2.resume_title_replay(arg_18_0, arg_18_1, arg_18_2, arg_18_3, arg_18_4, arg_18_5, arg_18_6, arg_18_7, arg_18_8, arg_18_9)
	local var_18_0 = mcp.make_command("resume-game", arg_18_0, arg_18_3.mode)

	var_18_0.rollback_mode = "replay"
	var_18_0.load_rollbackdata_path = arg_18_1
	var_18_0.save_rollbackdata_path = arg_18_2
	var_18_0.load_time_filename = arg_18_5
	var_18_0.save_time_filename = arg_18_6
	var_18_0.sram_filename = arg_18_4
	var_18_0.screenshot_filename = arg_18_7
	var_18_0.decorative_filename = arg_18_8
	var_18_0.raw_options = concat_options(arg_18_9)

	mcp.write_command(var_18_0)
	Application.exit()
end

function mcp2.launch_production_test_menu()
	mcp.write_command({
		command = "start-production-test-menu"
	})
	Application.exit()
end
