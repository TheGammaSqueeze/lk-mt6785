require("../core/core.lua")
require("../gui/gui.lua")

local var_0_0 = {
	"left",
	"right",
	"up",
	"down",
	"btn_a",
	"btn_b",
	"btn_y",
	"btn_x",
	"btn_l",
	"btn_r",
	"btn_start",
	"btn_select"
}

local function var_0_1(arg_1_0)
	for iter_1_0 = 1, #var_0_0 do
		if arg_1_0 == var_0_0[iter_1_0] then
			return true
		end
	end

	return false
end

local var_0_2 = {
	"btn_a",
	"btn_start",
	"btn_b",
	"btn_y",
	"btn_x",
	"btn_select"
}
local var_0_3 = {
	"left",
	"right",
	"up",
	"down"
}
local var_0_4 = {
	left = DPAD_LEFT,
	right = DPAD_RIGHT,
	up = DPAD_UP,
	down = DPAD_DOWN
}
local var_0_5 = {
	left = function(arg_2_0)
		return GUI:isControlPressedRepeat(arg_2_0, DPAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(arg_2_0, GAMEPAD_AXIS_X, -GUI.AXIS_THRESHOLD)
	end,
	right = function(arg_3_0)
		return GUI:isControlPressedRepeat(arg_3_0, DPAD_RIGHT, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(arg_3_0, GAMEPAD_AXIS_X, GUI.AXIS_THRESHOLD)
	end,
	up = function(arg_4_0)
		return GUI:isControlPressedRepeat(arg_4_0, DPAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(arg_4_0, GAMEPAD_AXIS_Y, GUI.AXIS_THRESHOLD)
	end,
	down = function(arg_5_0)
		return GUI:isControlPressedRepeat(arg_5_0, DPAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlPressedRepeat(arg_5_0, GAMEPAD_AXIS_Y, -GUI.AXIS_THRESHOLD)
	end
}

CloverPadFlags = class()

function CloverPadFlags.reset(arg_6_0)
	for iter_6_0 = 1, #var_0_0 do
		arg_6_0[var_0_0[iter_6_0]] = false
	end
end

function CloverPadFlags.copyFrom(arg_7_0, arg_7_1)
	for iter_7_0 = 1, #var_0_0 do
		arg_7_0[var_0_0[iter_7_0]] = arg_7_1[var_0_0[iter_7_0]]
	end
end

function CloverPadFlags.addFrom(arg_8_0, arg_8_1)
	for iter_8_0 = 1, #var_0_0 do
		local var_8_0 = var_0_0[iter_8_0]

		arg_8_0[var_8_0] = arg_8_0[var_8_0] or arg_8_1[var_8_0]
	end
end

function CloverPadFlags.isAnything(arg_9_0)
	for iter_9_0 = 1, #var_0_0 do
		if arg_9_0[var_0_0[iter_9_0]] then
			return true
		end
	end

	return false
end

function CloverPadFlags.isAnythingDecideKey(arg_10_0)
	for iter_10_0 = 1, #var_0_2 do
		if arg_10_0[var_0_2[iter_10_0]] then
			return true
		end
	end

	return false
end

function CloverPadFlags.isAnythingCursorKey(arg_11_0)
	for iter_11_0 = 1, #var_0_3 do
		if arg_11_0[var_0_3[iter_11_0]] then
			return true
		end
	end

	return false
end

CloverPadBase = class()

function CloverPadBase.init(arg_12_0)
	arg_12_0.pressed = new(CloverPadFlags)
	arg_12_0.down = new(CloverPadFlags)
	arg_12_0.release = new(CloverPadFlags)

	arg_12_0:reset()

	arg_12_0.disable_cnt = 0
	arg_12_0.game_device = GAME_DEVICE
end

function CloverPadBase.reset(arg_13_0)
	arg_13_0:resetInput()
	arg_13_0:resetNoInputTime()
end

function CloverPadBase.resetInput(arg_14_0)
	arg_14_0.pressed:reset()
	arg_14_0.down:reset()
	arg_14_0.release:reset()
end

function CloverPadBase.resetNoInputTime(arg_15_0)
	arg_15_0.no_input_time = 0
end

function CloverPadBase.update(arg_16_0)
	arg_16_0.pressed.left = GUI:isControlPressed(arg_16_0.game_device, DPAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_AXIS_X, -GUI.AXIS_THRESHOLD)
	arg_16_0.pressed.right = GUI:isControlPressed(arg_16_0.game_device, DPAD_RIGHT, GUI.KEY_THRESHOLD) or GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_AXIS_X, GUI.AXIS_THRESHOLD)
	arg_16_0.pressed.up = GUI:isControlPressed(arg_16_0.game_device, DPAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_AXIS_Y, GUI.AXIS_THRESHOLD)
	arg_16_0.pressed.down = GUI:isControlPressed(arg_16_0.game_device, DPAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_AXIS_Y, -GUI.AXIS_THRESHOLD)
	arg_16_0.pressed.btn_a = GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_A, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_b = GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_B, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_y = GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_Y, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_x = GUI:isControlPressed(arg_16_0.game_device, GAMEPAD_X, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_l = GUI:isControlPressed(arg_16_0.game_device, BUTTON_LEFT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_r = GUI:isControlPressed(arg_16_0.game_device, BUTTON_RIGHT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_start = GUI:isControlPressed(arg_16_0.game_device, BUTTON_START, GUI.KEY_THRESHOLD)
	arg_16_0.pressed.btn_select = GUI:isControlPressed(arg_16_0.game_device, BUTTON_SELECT, GUI.KEY_THRESHOLD)
	arg_16_0.down.left = GUI:isControlDown(arg_16_0.game_device, DPAD_LEFT, GUI.KEY_THRESHOLD) or GUI:isControlDown(arg_16_0.game_device, GAMEPAD_AXIS_X, -GUI.AXIS_THRESHOLD)
	arg_16_0.down.right = GUI:isControlDown(arg_16_0.game_device, DPAD_RIGHT, GUI.KEY_THRESHOLD) or GUI:isControlDown(arg_16_0.game_device, GAMEPAD_AXIS_X, GUI.AXIS_THRESHOLD)
	arg_16_0.down.up = GUI:isControlDown(arg_16_0.game_device, DPAD_UP, GUI.KEY_THRESHOLD) or GUI:isControlDown(arg_16_0.game_device, GAMEPAD_AXIS_Y, GUI.AXIS_THRESHOLD)
	arg_16_0.down.down = GUI:isControlDown(arg_16_0.game_device, DPAD_DOWN, GUI.KEY_THRESHOLD) or GUI:isControlDown(arg_16_0.game_device, GAMEPAD_AXIS_Y, -GUI.AXIS_THRESHOLD)
	arg_16_0.down.btn_a = GUI:isControlDown(arg_16_0.game_device, GAMEPAD_A, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_b = GUI:isControlDown(arg_16_0.game_device, GAMEPAD_B, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_y = GUI:isControlDown(arg_16_0.game_device, GAMEPAD_Y, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_x = GUI:isControlDown(arg_16_0.game_device, GAMEPAD_X, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_l = GUI:isControlDown(arg_16_0.game_device, BUTTON_LEFT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_r = GUI:isControlDown(arg_16_0.game_device, BUTTON_RIGHT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_start = GUI:isControlDown(arg_16_0.game_device, BUTTON_START, GUI.KEY_THRESHOLD)
	arg_16_0.down.btn_select = GUI:isControlDown(arg_16_0.game_device, BUTTON_SELECT, GUI.KEY_THRESHOLD)
	arg_16_0.release.left = GUI:isControlReleased(arg_16_0.game_device, DPAD_LEFT, GUI.KEY_THRESHOLD)
	arg_16_0.release.right = GUI:isControlReleased(arg_16_0.game_device, DPAD_RIGHT, GUI.KEY_THRESHOLD)
	arg_16_0.release.up = GUI:isControlReleased(arg_16_0.game_device, DPAD_UP, GUI.KEY_THRESHOLD)
	arg_16_0.release.down = GUI:isControlReleased(arg_16_0.game_device, DPAD_DOWN, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_a = GUI:isControlReleased(arg_16_0.game_device, GAMEPAD_A, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_b = GUI:isControlReleased(arg_16_0.game_device, GAMEPAD_B, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_y = GUI:isControlReleased(arg_16_0.game_device, GAMEPAD_Y, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_x = GUI:isControlReleased(arg_16_0.game_device, GAMEPAD_X, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_l = GUI:isControlReleased(arg_16_0.game_device, BUTTON_LEFT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_r = GUI:isControlReleased(arg_16_0.game_device, BUTTON_RIGHT_TRIGGER, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_start = GUI:isControlReleased(arg_16_0.game_device, BUTTON_START, GUI.KEY_THRESHOLD)
	arg_16_0.release.btn_select = GUI:isControlReleased(arg_16_0.game_device, BUTTON_SELECT, GUI.KEY_THRESHOLD)
end

function CloverPadBase.doUpdate(arg_17_0, arg_17_1)
	if arg_17_0:isEnable() then
		arg_17_0:update(arg_17_1)

		if arg_17_0.down:isAnything() then
			arg_17_0.no_input_time = 0
		else
			arg_17_0.no_input_time = arg_17_0.no_input_time + arg_17_1
		end
	end
end

function CloverPadBase.copyFrom(arg_18_0, arg_18_1)
	arg_18_0.pressed:copyFrom(arg_18_1.pressed)
	arg_18_0.down:copyFrom(arg_18_1.down)
	arg_18_0.release:copyFrom(arg_18_1.release)
end

function CloverPadBase.addFrom(arg_19_0, arg_19_1)
	arg_19_0.pressed:addFrom(arg_19_1.pressed)
	arg_19_0.down:addFrom(arg_19_1.down)
	arg_19_0.release:addFrom(arg_19_1.release)
end

function CloverPadBase.disable(arg_20_0)
	arg_20_0.disable_cnt = arg_20_0.disable_cnt + 1

	if arg_20_0.disable_cnt == 1 then
		arg_20_0:reset()
	end
end

function CloverPadBase.enable(arg_21_0)
	if arg_21_0.disable_cnt == 0 then
		return
	end

	arg_21_0.disable_cnt = arg_21_0.disable_cnt - 1

	if arg_21_0.disable_cnt == 0 then
		arg_21_0:reset()
	end
end

function CloverPadBase.forceEnable(arg_22_0)
	arg_22_0.disable_cnt = 0
end

function CloverPadBase.isEnable(arg_23_0)
	return arg_23_0.disable_cnt == 0
end

function CloverPadBase.getNoInputTime(arg_24_0)
	return arg_24_0.no_input_time
end

CloverPad1P = class(CloverPadBase)

function CloverPad1P.init(arg_25_0)
	CloverPadBase.init(arg_25_0)

	if HOST_PLATFORM_IS_WINDOWS then
		arg_25_0.game_device = GAME_DEVICE
	else
		arg_25_0.game_device = GAME_DEVICE_1
	end
end

CloverPad2P = class(CloverPadBase)

function CloverPad2P.init(arg_26_0)
	CloverPadBase.init(arg_26_0)

	arg_26_0.game_device = GAME_DEVICE_2
end

CloverPadAuto = class(CloverPadBase)

function CloverPadAuto.init(arg_27_0)
	CloverPadBase.init(arg_27_0)

	arg_27_0.request = {}

	arg_27_0:resetRequest()
end

function CloverPadAuto.update(arg_28_0)
	local var_28_0 = new(CloverPadFlags)

	var_28_0:copyFrom(arg_28_0.down)
	arg_28_0:resetInput()

	for iter_28_0 = 1, #var_0_0 do
		local var_28_1 = var_0_0[iter_28_0]

		if arg_28_0.request[var_28_1] > 0 then
			arg_28_0.down[var_28_1] = true
		end
	end

	for iter_28_1 = 1, #var_0_0 do
		local var_28_2 = var_0_0[iter_28_1]

		if var_28_0[var_28_2] ~= arg_28_0.down[var_28_2] then
			if var_28_0[var_28_2] then
				arg_28_0.release[var_28_2] = true
			else
				arg_28_0.pressed[var_28_2] = true
			end
		end
	end

	for iter_28_2 = 1, #var_0_0 do
		local var_28_3 = var_0_0[iter_28_2]

		if arg_28_0.request[var_28_3] ~= 0 then
			arg_28_0.request[var_28_3] = arg_28_0.request[var_28_3] - 1

			if arg_28_0.request[var_28_3] <= 0 then
				arg_28_0.request[var_28_3] = 0
			end
		end
	end
end

function CloverPadAuto.resetRequest(arg_29_0)
	for iter_29_0 = 1, #var_0_0 do
		arg_29_0.request[var_0_0[iter_29_0]] = 0
	end
end

function CloverPadAuto.inputRequest(arg_30_0, arg_30_1, arg_30_2)
	assert(var_0_1(arg_30_1))

	if arg_30_2 == nil then
		arg_30_2 = 1
	end

	arg_30_0.request[arg_30_1] = arg_30_2
end

CloverPadUI = class(CloverPadBase)

function CloverPadUI.init(arg_31_0)
	CloverPadBase.init(arg_31_0)

	arg_31_0.is_enable_user = true
	arg_31_0.disable_id_list = {}
	arg_31_0.is_enable_auto = true
	arg_31_0.priority_input = ""
end

function CloverPadUI.update(arg_32_0)
	if arg_32_0.is_enable_user then
		arg_32_0:copyFrom(CloverPad1P)
	else
		arg_32_0:resetInput()
	end

	if arg_32_0.is_enable_auto then
		arg_32_0:addFrom(CloverPadAuto)
	end

	if arg_32_0.pressed:isAnythingDecideKey() then
		local var_32_0 = false

		for iter_32_0 = 1, #var_0_2 do
			if var_32_0 then
				arg_32_0.pressed[var_0_2[iter_32_0]] = false
			elseif arg_32_0.pressed[var_0_2[iter_32_0]] then
				var_32_0 = true
			end
		end

		for iter_32_1 = 1, #var_0_3 do
			arg_32_0.pressed[var_0_3[iter_32_1]] = false
		end
	end

	if arg_32_0.down[arg_32_0.priority_input] then
		for iter_32_2 = 1, #var_0_3 do
			arg_32_0.pressed[var_0_3[iter_32_2]] = false
		end

		arg_32_0:updateRepeat(arg_32_0.priority_input)
	else
		arg_32_0.priority_input = ""

		if arg_32_0.pressed:isAnythingCursorKey() then
			local var_32_1 = false

			for iter_32_3 = 1, #var_0_3 do
				if var_32_1 then
					arg_32_0.pressed[var_0_3[iter_32_3]] = false
				elseif arg_32_0.pressed[var_0_3[iter_32_3]] then
					var_32_1 = true
					arg_32_0.priority_input = var_0_3[iter_32_3]

					arg_32_0:updateRepeat(arg_32_0.priority_input)
				end
			end
		end
	end
end

function CloverPadUI.updateRepeat(arg_33_0, arg_33_1)
	if GUI.H.REPEAT_OFF then
		if arg_33_1 == "left" or arg_33_1 == "right" then
			return
		end
	elseif GUI.V.REPEAT_OFF and (arg_33_1 == "up" or arg_33_1 == "down") then
		return
	end

	if CloverPad1P.down[arg_33_1] then
		local var_33_0 = var_0_5[arg_33_1]

		if var_33_0 then
			arg_33_0.pressed[arg_33_1] = var_33_0(CloverPad1P.game_device)
		end
	end
end

function CloverPadUI.EnableUserInput(arg_34_0, arg_34_1, arg_34_2)
	arg_34_2 = arg_34_2 or "default"

	if arg_34_1 then
		arg_34_0.disable_id_list[arg_34_2] = nil
	else
		arg_34_0.disable_id_list[arg_34_2] = true
	end

	if not next(arg_34_0.disable_id_list) then
		arg_34_0.is_enable_user = true
	else
		arg_34_0.is_enable_user = false
	end

	local var_34_0 = "** enable input(" .. arg_34_2 .. "/" .. tostring(arg_34_1) .. "):"

	for iter_34_0, iter_34_1 in pairs(arg_34_0.disable_id_list) do
		var_34_0 = var_34_0 .. iter_34_0 .. ", "
	end

	print(var_34_0)
end

function CloverPadUI.EnableAutoInput(arg_35_0, arg_35_1)
	arg_35_0.is_enable_auto = arg_35_1
end

function CloverPadUI.isCancelPressed(arg_36_0)
	return arg_36_0.pressed.btn_b
end

function CloverPadUI.isCancelDown(arg_37_0)
	return arg_37_0.down.btn_b
end

function CloverPadUI.isCancelReleased(arg_38_0)
	return arg_38_0.release.btn_b
end

function CloverPadUI.isValidatePressed(arg_39_0)
	return arg_39_0.pressed.btn_a
end

function CloverPadUI.isValidateDown(arg_40_0)
	return arg_40_0.down.btn_a
end

function CloverPadUI.isValidateReleased(arg_41_0)
	return arg_41_0.release.btn_a
end

CloverPadDbg = class(CloverPadBase)

function CloverPadDbg.update(arg_42_0)
	if REED_DEBUG then
		arg_42_0:copyFrom(CloverPad1P)
	end
end

Pad = {
	register_btn = "btn_y",
	validate_btn = "btn_a",
	cancel_btn = "btn_b",
	Init = function()
		CloverPad1P:init()
		CloverPad2P:init()
		CloverPadAuto:init()
		CloverPadUI:init()
		CloverPadDbg:init()

		function GUI.isAnythingPressed(arg_44_0)
			return CloverPadUI.pressed:isAnything()
		end

		function GUI.isLeftPressed(arg_45_0)
			return CloverPadUI.pressed.left
		end

		function GUI.isRightPressed(arg_46_0)
			return CloverPadUI.pressed.right
		end

		function GUI.isUpPressed(arg_47_0)
			return CloverPadUI.pressed.up
		end

		function GUI.isDownPressed(arg_48_0)
			return CloverPadUI.pressed.down
		end

		function GUI.isValidatePressed(arg_49_0)
			return CloverPadUI:isValidatePressed()
		end

		function GUI.isValidateDown(arg_50_0)
			return CloverPadUI:isValidateDown()
		end

		function GUI.isValidateReleased(arg_51_0)
			return CloverPadUI:isValidateReleased()
		end
	end,
	Update = function(arg_52_0)
		CloverPad1P:doUpdate(arg_52_0)
		CloverPad2P:doUpdate(arg_52_0)
		CloverPadAuto:doUpdate(arg_52_0)
		CloverPadUI:doUpdate(arg_52_0)
		CloverPadDbg:doUpdate(arg_52_0)
	end,
	GetPadList = function()
		return var_0_0
	end
}
