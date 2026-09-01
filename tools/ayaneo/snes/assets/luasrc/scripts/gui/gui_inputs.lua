require("gui.lua")

GUI.AXIS_THRESHOLD = math.cos(math.pi / 4)
GUI.TOUCH_THRESHOLD = 0.2
GUI.KEY_THRESHOLD = 0.1
GUI.REPEAT_DELAY = 0.5
GUI.REPEAT_RATE = 0.1
GUI.currentTime = 0
GUI.startPressedTime = 0
GUI.state = 0
GUI.devicePressed = 0
GUI.controlPressed = 0

function GUI.isControlPressed(arg_1_0, arg_1_1, arg_1_2, arg_1_3)
	return arg_1_3 >= 0 and arg_1_3 <= Input.getState(arg_1_1, arg_1_2) and arg_1_3 > Input.getPreviousState(arg_1_1, arg_1_2) or arg_1_3 < 0 and arg_1_3 >= Input.getState(arg_1_1, arg_1_2) and arg_1_3 < Input.getPreviousState(arg_1_1, arg_1_2)
end

function GUI.isControlReleased(arg_2_0, arg_2_1, arg_2_2, arg_2_3)
	return arg_2_3 >= 0 and arg_2_3 >= Input.getState(arg_2_1, arg_2_2) and arg_2_3 < Input.getPreviousState(arg_2_1, arg_2_2) or arg_2_3 < 0 and arg_2_3 <= Input.getState(arg_2_1, arg_2_2) and arg_2_3 > Input.getPreviousState(arg_2_1, arg_2_2)
end

function GUI.isControlDown(arg_3_0, arg_3_1, arg_3_2, arg_3_3)
	return arg_3_3 >= 0 and arg_3_3 < Input.getState(arg_3_1, arg_3_2) or arg_3_3 < 0 and arg_3_3 > Input.getState(arg_3_1, arg_3_2)
end

function GUI.isControlUp(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
	return arg_4_3 >= 0 and arg_4_3 >= Input.getState(arg_4_1, arg_4_2) or arg_4_3 < 0 and arg_4_3 <= Input.getState(arg_4_1, arg_4_2)
end

function GUI.wasControlDown(arg_5_0, arg_5_1, arg_5_2, arg_5_3)
	return arg_5_3 >= 0 and arg_5_3 < Input.getPreviousState(arg_5_1, arg_5_2) or arg_5_3 < 0 and arg_5_3 > Input.getPreviousState(arg_5_1, arg_5_2)
end

function GUI.wasControlUp(arg_6_0, arg_6_1, arg_6_2, arg_6_3)
	return arg_6_3 >= 0 and arg_6_3 >= Input.getPreviousState(arg_6_1, arg_6_2) or arg_6_3 < 0 and arg_6_3 <= Input.getPreviousState(arg_6_1, arg_6_2)
end

function GUI.isControlPressedRepeat(arg_7_0, arg_7_1, arg_7_2, arg_7_3)
	local var_7_0 = GUI:isControlPressed(arg_7_1, arg_7_2, arg_7_3)
	local var_7_1 = GUI:isControlDown(arg_7_1, arg_7_2, arg_7_3)
	local var_7_2 = arg_7_3 >= 0

	if var_7_0 then
		GUI.controlPressed = arg_7_2
		GUI.devicePressed = arg_7_1
		GUI.orientation = var_7_2
		GUI.state = 1
		GUI.startPressedTime = GUI.currentTime

		if GUI.DEBUG_LEVEL.input_repeat then
			GUI:debugPrint("state 0 : pressed")
		end

		return true
	end

	if GUI.state ~= 0 and GUI.devicePressed == arg_7_1 and GUI.controlPressed == arg_7_2 and GUI.orientation == var_7_2 then
		if GUI.repeatedThisFrame then
			return true
		end

		if GUI.state == 1 then
			if not var_7_1 then
				if GUI.DEBUG_LEVEL.input_repeat then
					GUI:debugPrint("state 1 : not down")
				end

				GUI.state = 0
			elseif GUI.currentTime - GUI.startPressedTime > GUI.REPEAT_DELAY then
				GUI.state = 2
				GUI.startPressedTime = GUI.currentTime

				if GUI.DEBUG_LEVEL.input_repeat then
					GUI:debugPrint("state 1 : repeat delay reached")
				end

				GUI.repeatedThisFrame = true

				return true
			end
		elseif GUI.state == 2 then
			if not var_7_1 then
				if GUI.DEBUG_LEVEL.input_repeat then
					GUI:debugPrint("state 2 : not down")
				end

				GUI.state = 0
			elseif GUI.currentTime - GUI.startPressedTime > GUI.REPEAT_RATE then
				GUI.startPressedTime = GUI.currentTime

				if GUI.DEBUG_LEVEL.input_repeat then
					GUI:debugPrint("state 2 : repeat rate reached")
				end

				GUI.repeatedThisFrame = true

				return true
			end
		end
	end

	return false
end

function GUI.isKeyPressed(arg_8_0, arg_8_1, arg_8_2)
	return GUI:isControlPressed(arg_8_1, arg_8_2, GUI.KEY_THRESHOLD)
end

function GUI.isKeyPressedRepeat(arg_9_0, arg_9_1, arg_9_2)
	return GUI:isControlPressedRepeat(arg_9_1, arg_9_2, GUI.KEY_THRESHOLD)
end

function GUI.isKeyReleased(arg_10_0, arg_10_1, arg_10_2)
	return GUI:isControlReleased(arg_10_1, arg_10_2, GUI.KEY_THRESHOLD)
end

function GUI.isTouched(arg_11_0)
	return GUI:isControlDown(TOUCH_DEVICE, TOUCH_TOUCHED, GUI.TOUCH_THRESHOLD)
end

function GUI.wasTouched(arg_12_0)
	return GUI:wasControlDown(TOUCH_DEVICE, TOUCH_TOUCHED, GUI.TOUCH_THRESHOLD)
end

function GUI.isLeftDown(arg_13_0)
	return GUI:isControlDown(GAME_DEVICE, GAMEPAD_PAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_AXIS_X, -GUI.AXIS_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_RAXIS_X, -GUI.AXIS_THRESHOLD)
end

function GUI.isLeftPressed(arg_14_0)
	return GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_PAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_AXIS_X, -GUI.AXIS_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_RAXIS_X, -GUI.AXIS_THRESHOLD)
end

function GUI.isRightDown(arg_15_0)
	return GUI:isControlDown(GAME_DEVICE, GAMEPAD_PAD_RIGHT, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_AXIS_X, GUI.AXIS_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_RAXIS_X, GUI.AXIS_THRESHOLD)
end

function GUI.isRightPressed(arg_16_0)
	return GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_PAD_RIGHT, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_AXIS_X, GUI.AXIS_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_RAXIS_X, GUI.AXIS_THRESHOLD)
end

function GUI.isUpDown(arg_17_0)
	return GUI:isControlDown(GAME_DEVICE, GAMEPAD_PAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_AXIS_Y, GUI.AXIS_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_RAXIS_Y, GUI.AXIS_THRESHOLD)
end

function GUI.isUpPressed(arg_18_0)
	return GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_PAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_AXIS_Y, GUI.AXIS_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_RAXIS_Y, GUI.AXIS_THRESHOLD)
end

function GUI.isDownDown(arg_19_0)
	return GUI:isControlDown(GAME_DEVICE, GAMEPAD_PAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_AXIS_Y, -GUI.AXIS_THRESHOLD) or GUI:isControlDown(GAME_DEVICE, GAMEPAD_RAXIS_Y, -GUI.AXIS_THRESHOLD)
end

function GUI.isDownPressed(arg_20_0)
	return GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_PAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_AXIS_Y, -GUI.AXIS_THRESHOLD) or GUI:isControlPressedRepeat(GAME_DEVICE, GAMEPAD_RAXIS_Y, -GUI.AXIS_THRESHOLD)
end

function GUI.isValidateDown(arg_21_0)
	return not GUI:isTouched() and GUI:isControlDown(GAME_DEVICE, GAMEPAD_A, GUI.KEY_THRESHOLD)
end

function GUI.isValidatePressed(arg_22_0)
	return GUI:isKeyPressed(GAME_DEVICE, GAMEPAD_A)
end

function GUI.isValidateReleased(arg_23_0)
	return GUI:isControlReleased(GAME_DEVICE, GAMEPAD_A, GUI.KEY_THRESHOLD)
end

function GUI.isCancelPressed(arg_24_0)
	return GUI:isKeyPressed(GAME_DEVICE, GAMEPAD_B)
end

function GUI.isAnythingPressed(arg_25_0)
	return arg_25_0:isLeftPressed() or arg_25_0:isRightPressed() or arg_25_0:isUpPressed() or arg_25_0:isDownPressed() or arg_25_0:isValidatePressed() or arg_25_0:isCancelPressed() or arg_25_0:isTouched()
end
