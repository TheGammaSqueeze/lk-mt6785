require("/scripts/core/core.lua")

test_program = class()
test_program.seq = 0
test_program.t = 0

function test_program.update(arg_1_0, arg_1_1)
	local var_1_0 = GUI:isControlDown(GAME_DEVICE_1, BUTTON_START, GUI.KEY_THRESHOLD) and GUI:isControlDown(GAME_DEVICE_1, BUTTON_SELECT, GUI.KEY_THRESHOLD)
	local var_1_1 = GUI:isControlDown(GAME_DEVICE_2, BUTTON_A, GUI.KEY_THRESHOLD) and GUI:isControlDown(GAME_DEVICE_2, BUTTON_B, GUI.KEY_THRESHOLD) and GUI:isControlDown(GAME_DEVICE_2, DPAD_LEFT, GUI.KEY_THRESHOLD)
	local var_1_2 = GUI:isControlDown(GAME_DEVICE_2, BUTTON_A, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_B, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, DPAD_LEFT, GUI.KEY_THRESHOLD)
	local var_1_3 = GUI:isControlDown(GAME_DEVICE_1, BUTTON_A, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, BUTTON_B, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, BUTTON_X, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, BUTTON_Y, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, BUTTON_LEFT_TRIGGER, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, BUTTON_RIGHT_TRIGGER, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, DPAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, DPAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, DPAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_1, DPAD_RIGHT, GUI.KEY_THRESHOLD)
	local var_1_4 = GUI:isControlDown(GAME_DEVICE_2, BUTTON_X, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_Y, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_LEFT_TRIGGER, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_RIGHT_TRIGGER, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_START, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, BUTTON_SELECT, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, DPAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, DPAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE_2, DPAD_RIGHT, GUI.KEY_THRESHOLD)

	if var_1_3 or var_1_4 then
		test_program.seq = 0
	end

	if var_1_0 then
		if test_program.seq == 0 and not var_1_2 then
			test_program.seq = 1
		end
	else
		test_program.seq = 0
		test_program.t = 0
	end

	if test_program.seq == 1 and var_1_1 then
		test_program.t = test_program.t + arg_1_1

		if test_program.t > 10 then
			test_program.seq = 2
		end
	else
		test_program.t = 0
	end

	if test_program.seq == 2 then
		test_program:run()

		test_program.seq = 3
	end
end

function test_program.run(arg_2_0)
	if HOST_PLATFORM_IS_WINDOWS then
		system.fadeOut()

		return
	end

	mcp2.launch_production_test_menu()
end
