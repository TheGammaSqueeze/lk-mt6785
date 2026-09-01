require("../core/core.lua")

GUI_COMMAND_FOCUS = "COMMAND_FOCUS"
GUI_COMMAND_UNFOCUS = "COMMAND_UNFOCUS"
GUI_COMMAND_PRESS = "COMMAND_PRESS"
GUI_COMMAND_DRAG = "COMMAND_DRAG"
GUI_COMMAND_RELEASE = "COMMAND_RELEASE"
GUI_COMMAND_CLICK = "COMMAND_CLICK"
GUI_COMMAND_LEFT = "COMMAND_LEFT"
GUI_COMMAND_RIGHT = "COMMAND_RIGHT"
GUI_COMMAND_UP = "COMMAND_UP"
GUI_COMMAND_DOWN = "COMMAND_DOWN"
GUI_COMMAND_KEYPRESS = "COMMAND_KEYPRESS"
GUI = class()
GUI.pressedCommandTarget = nil
GUI.focusedElement = nil
GUI.clickMode = "release"
GUI.buttonMode = "push"
GUI.dragThresholdX = 4
GUI.dragThresholdY = 4
GUI.hasAutomaticNavigation = false
GUI.roundedUnits = false
GUI_NEIGHBOUR_SELECTIVITY = 0.5
GUI.DEBUG_LEVEL = {
	hit_test = false,
	actions = false,
	runtime = false,
	commands = false,
	inputs = false,
	callbacks = false,
	states = false,
	sounds = false,
	dragging = false,
	scroll_layout = false,
	neighbors = false,
	hit_test_ll = false,
	animations = false,
	info = false,
	focus = false,
	input_repeat = false
}

local var_0_0 = pairs
local var_0_1 = {}

local function var_0_2(arg_1_0)
	return {
		id = arg_1_0
	}
end

local function var_0_3(arg_2_0)
	local var_2_0 = var_0_2(GUI_COMMAND_FOCUS)

	var_2_0.nosound = arg_2_0

	return var_2_0
end

local function var_0_4(arg_3_0, arg_3_1, arg_3_2, arg_3_3)
	local var_3_0 = var_0_2(GUI_COMMAND_RELEASE)

	var_3_0.posX = arg_3_0
	var_3_0.posY = arg_3_1
	var_3_0.endDrag = arg_3_2
	var_3_0.overstep = arg_3_3

	return var_3_0
end

local function var_0_5(arg_4_0, arg_4_1)
	local var_4_0 = var_0_2(GUI_COMMAND_CLICK)

	var_4_0.posX = arg_4_0
	var_4_0.posY = arg_4_1

	return var_4_0
end

local function var_0_6(arg_5_0, arg_5_1, arg_5_2, arg_5_3, arg_5_4)
	local var_5_0 = var_0_2(GUI_COMMAND_DRAG)

	var_5_0.previousPosX = arg_5_0
	var_5_0.previousPosY = arg_5_1
	var_5_0.posX = arg_5_2
	var_5_0.posY = arg_5_3
	var_5_0.first = arg_5_4

	return var_5_0
end

function initializeElementSound(arg_6_0)
	if arg_6_0 then
		arg_6_0:stop()
		arg_6_0:setAutoPlay(false)
	end
end

function GUI.start(arg_7_0)
	if GUI.manager ~= nil then
		GUI:error("Trying to start GUI manager, but one already exists ('" .. GUI.manager.worldNode:getName() .. "')")
	end

	GUI.manager = arg_7_0

	if arg_7_0.hasAutomaticNavigation ~= nil then
		GUI.hasAutomaticNavigation = arg_7_0.hasAutomaticNavigation
	end

	if arg_7_0.dragThresholdX ~= nil then
		GUI.dragThresholdX = arg_7_0.dragThresholdX
	end

	if arg_7_0.dragThresholdY ~= nil then
		GUI.dragThresholdY = arg_7_0.dragThresholdY
	end

	if arg_7_0.roundedUnits ~= nil then
		GUI.roundedUnits = arg_7_0.roundedUnits
	end

	if arg_7_0.repeatDelay ~= nil then
		GUI.REPEAT_DELAY = arg_7_0.repeatDelay
	end

	if arg_7_0.repeatRate ~= nil then
		GUI.REPEAT_RATE = arg_7_0.repeatRate
	end

	GUI:reset()
	GUI:setInputCamera(arg_7_0.inputCamera)
end

function GUI.stop(arg_8_0)
	GUI.manager = nil
end

function GUI.update(arg_9_0, arg_9_1)
	GUI:updateInputs(arg_9_1)
end

function GUI.reset(arg_10_0)
	arg_10_0.pressedCommandTarget = nil
	arg_10_0.focusedElement = nil
	arg_10_0.buttonTable = nil
end

function GUI.debugPrint(arg_11_0, arg_11_1, arg_11_2)
	if not arg_11_2 or GUI.DEBUG_LEVEL[arg_11_2] == true then
		print("<GUI> " .. arg_11_1)
	end
end

function GUI.warning(arg_12_0, arg_12_1)
	print("<GUI> <WARNING> " .. arg_12_1)
end

function GUI.error(arg_13_0, arg_13_1)
	error("<GUI> <ERROR> " .. arg_13_1)
end

function GUI.registerButton(arg_14_0, arg_14_1)
	if arg_14_0.buttonTable == nil then
		arg_14_0.buttonTable = {}
	end

	arg_14_0.buttonTable[arg_14_1] = true
end

function GUI.unregisterButton(arg_15_0, arg_15_1)
	if arg_15_0.buttonTable then
		arg_15_0.buttonTable[arg_15_1] = nil
	end
end

function GUI.getButtonCenter(arg_16_0, arg_16_1)
	local var_16_0, var_16_1 = arg_16_1:getWorldPosition()
	local var_16_2, var_16_3 = arg_16_1.clickable:getCenter()
	local var_16_4 = var_16_0 + var_16_2
	local var_16_5 = var_16_1 + var_16_3
	local var_16_6, var_16_7 = arg_16_0.camera:worldToScreenPosition(var_16_4, var_16_5)
	local var_16_8 = var_16_7

	return var_16_6, var_16_8
end

function GUI.getButtonCorners(arg_17_0, arg_17_1, arg_17_2)
	local var_17_0, var_17_1 = arg_17_1:getWorldPosition()
	local var_17_2, var_17_3 = arg_17_1.clickable:getCenter()
	local var_17_4, var_17_5 = arg_17_1.clickable:getBoxShapeSize()
	local var_17_6 = var_17_0 + var_17_2
	local var_17_7 = var_17_1 + var_17_3
	local var_17_8, var_17_9 = arg_17_0.camera:worldToScreenPosition(var_17_6 + var_17_4 / 2, var_17_7 + var_17_5 / 2)

	table.insert(arg_17_2, {
		var_17_8,
		var_17_9
	})

	local var_17_10, var_17_11 = arg_17_0.camera:worldToScreenPosition(var_17_6 + var_17_4 / 2, var_17_7 - var_17_5 / 2)

	table.insert(arg_17_2, {
		var_17_10,
		var_17_11
	})

	local var_17_12, var_17_13 = arg_17_0.camera:worldToScreenPosition(var_17_6 - var_17_4 / 2, var_17_7 - var_17_5 / 2)

	table.insert(arg_17_2, {
		var_17_12,
		var_17_13
	})

	local var_17_14, var_17_15 = arg_17_0.camera:worldToScreenPosition(var_17_6 - var_17_4 / 2, var_17_7 + var_17_5 / 2)

	table.insert(arg_17_2, {
		var_17_14,
		var_17_15
	})
end

function GUI.getButtonNearestCorner(arg_18_0, arg_18_1, arg_18_2, arg_18_3)
	local var_18_0 = math.huge
	local var_18_1 = math.huge
	local var_18_2 = 0
	local var_18_3 = 0

	for iter_18_0 = 1, #arg_18_1 do
		local var_18_4 = math.abs(arg_18_1[iter_18_0][1] - arg_18_2)
		local var_18_5 = math.abs(arg_18_1[iter_18_0][2] - arg_18_3)

		if var_18_4 < var_18_0 then
			var_18_0 = var_18_4
			var_18_2 = arg_18_1[iter_18_0][1]
		end

		if var_18_5 < var_18_1 then
			var_18_1 = var_18_5
			var_18_3 = arg_18_1[iter_18_0][2]
		end
	end

	return var_18_2, var_18_3
end

function GUI.getMinMax(arg_19_0, arg_19_1)
	local var_19_0 = math.huge
	local var_19_1 = math.huge
	local var_19_2 = -math.huge
	local var_19_3 = -math.huge

	for iter_19_0 = 1, #arg_19_1 do
		local var_19_4 = arg_19_1[iter_19_0][1]
		local var_19_5 = arg_19_1[iter_19_0][2]

		if var_19_4 < var_19_0 then
			var_19_0 = var_19_4
		end

		if var_19_2 < var_19_4 then
			var_19_2 = var_19_4
		end

		if var_19_5 < var_19_1 then
			var_19_1 = var_19_5
		end

		if var_19_3 < var_19_5 then
			var_19_3 = var_19_5
		end
	end

	return {
		var_19_0,
		var_19_2,
		var_19_1,
		var_19_3
	}
end

function GUI.isInPartition(arg_20_0, arg_20_1, arg_20_2, arg_20_3)
	local var_20_0 = arg_20_0:getMinMax(arg_20_1)
	local var_20_1 = arg_20_0:getMinMax(arg_20_2)

	if arg_20_3 == "GUI_DIRECTION_LEFT" and var_20_0[2] < var_20_1[1] then
		return true
	elseif arg_20_3 == "GUI_DIRECTION_RIGHT" and var_20_0[1] > var_20_1[2] then
		return true
	elseif arg_20_3 == "GUI_DIRECTION_DOWN" and var_20_0[4] < var_20_1[3] then
		return true
	elseif arg_20_3 == "GUI_DIRECTION_UP" and var_20_0[3] > var_20_1[4] then
		return true
	else
		return false
	end
end

function GUI.findAutoNeighbour(arg_21_0, arg_21_1, arg_21_2, arg_21_3)
	if not GUI.hasAutomaticNavigation then
		return nil
	end

	local var_21_0
	local var_21_1 = math.huge
	local var_21_2 = {}

	arg_21_0:getButtonCorners(arg_21_2, var_21_2)

	local var_21_3, var_21_4 = arg_21_0:getButtonCenter(arg_21_2)

	for iter_21_0 in var_0_0(arg_21_1) do
		local var_21_5 = {}

		if iter_21_0:isEnabled() == true and iter_21_0:isAutoFocusDisabled() == false then
			arg_21_0:getButtonCorners(iter_21_0, var_21_5)

			if arg_21_0:isInPartition(var_21_5, var_21_2, arg_21_3) then
				local var_21_6, var_21_7 = arg_21_0:getButtonNearestCorner(var_21_5, var_21_3, var_21_4)
				local var_21_8 = 0
				local var_21_9 = math.abs(var_21_3 - var_21_6)
				local var_21_10 = math.abs(var_21_4 - var_21_7)

				if arg_21_3 == "GUI_DIRECTION_LEFT" or arg_21_3 == "GUI_DIRECTION_RIGHT" then
					var_21_8 = GUI_NEIGHBOUR_SELECTIVITY * var_21_9 + var_21_10
				else
					var_21_8 = GUI_NEIGHBOUR_SELECTIVITY * var_21_10 + var_21_9
				end

				if var_21_8 < var_21_1 then
					var_21_1 = var_21_8
					var_21_0 = iter_21_0
				end
			end
		end
	end

	if var_21_0 == nil then
		if GUI.DEBUG_LEVEL.neighbors then
			GUI:debugPrint("No neighbour found")
		end
	elseif GUI.DEBUG_LEVEL.neighbors then
		GUI:debugPrint("Neighbour button is " .. tostring(arg_21_2) .. " in " .. var_21_0:getName())
	end

	return var_21_0
end

function GUI.getNeighborElement(arg_22_0, arg_22_1, arg_22_2)
	local var_22_0

	if arg_22_2 == "GUI_DIRECTION_LEFT" then
		var_22_0 = arg_22_1.leftElement
	elseif arg_22_2 == "GUI_DIRECTION_RIGHT" then
		var_22_0 = arg_22_1.rightElement
	elseif arg_22_2 == "GUI_DIRECTION_DOWN" then
		var_22_0 = arg_22_1.downElement
	elseif arg_22_2 == "GUI_DIRECTION_UP" then
		var_22_0 = arg_22_1.upElement
	end

	while var_22_0 and var_22_0 ~= arg_22_1 and (var_22_0.currentState == "disabled" or not var_22_0:isEnabled()) do
		if arg_22_2 == "GUI_DIRECTION_LEFT" then
			var_22_0 = var_22_0.leftElement
		elseif arg_22_2 == "GUI_DIRECTION_RIGHT" then
			var_22_0 = var_22_0.rightElement
		elseif arg_22_2 == "GUI_DIRECTION_DOWN" then
			var_22_0 = var_22_0.downElement
		elseif arg_22_2 == "GUI_DIRECTION_UP" then
			var_22_0 = var_22_0.upElement
		end
	end

	if var_22_0 and var_22_0.currentState ~= "disabled" and var_22_0:isEnabled() then
		return var_22_0
	end

	if GUI.hasAutomaticNavigation then
		return arg_22_0:findAutoNeighbour(arg_22_0.buttonTable, arg_22_1, arg_22_2)
	end
end

function GUI.setInputCamera(arg_23_0, arg_23_1)
	arg_23_0.camera = arg_23_1

	local var_23_0 = arg_23_1:getTargetScreen()

	arg_23_0.screenWidth, arg_23_0.screenHeight = getScreenSize(var_23_0)
end

function GUI.inputToElementLocalPosition(arg_24_0, arg_24_1, arg_24_2, arg_24_3)
	local var_24_0 = arg_24_2
	local var_24_1 = arg_24_0.screenHeight - arg_24_3

	if GUI.DEBUG_LEVEL.hit_test_ll then
		GUI:debugPrint("screen pos = " .. tostring(var_24_0) .. ", " .. tostring(var_24_1))
	end

	local var_24_2, var_24_3 = arg_24_0.camera:screenToWorldPosition(var_24_0, var_24_1)

	return arg_24_1:worldToLocalPosition(var_24_2, var_24_3)
end

function GUI.getClickableAt(arg_25_0, arg_25_1, arg_25_2)
	if GUI.DEBUG_LEVEL.hit_test_ll then
		GUI:debugPrint("touch pos = " .. tostring(arg_25_1) .. ", " .. tostring(arg_25_2))
	end

	local var_25_0 = arg_25_1
	local var_25_1 = arg_25_0.screenHeight - arg_25_2

	if GUI.DEBUG_LEVEL.hit_test_ll then
		GUI:debugPrint("screen pos = " .. tostring(var_25_0) .. ", " .. tostring(var_25_1))
	end

	local var_25_2, var_25_3 = arg_25_0.camera:screenToWorldPosition(var_25_0, var_25_1)

	if GUI.DEBUG_LEVEL.hit_test_ll then
		GUI:debugPrint("world pos = " .. tostring(var_25_2) .. ", " .. tostring(var_25_3))
	end

	return ClickableComponent.raycastNearest(var_25_2, var_25_3), var_25_2, var_25_3
end

function GUI.getElementAt(arg_26_0, arg_26_1, arg_26_2)
	local var_26_0, var_26_1, var_26_2 = arg_26_0:getClickableAt(arg_26_1, arg_26_2)

	if var_26_0 ~= nil then
		if GUI.DEBUG_LEVEL.hit_test then
			GUI:debugPrint("hit clickable " .. tostring(var_26_0))
		end

		local var_26_3 = var_26_0:getClickListener()

		if var_26_3 ~= nil and var_26_3.isGUIElement then
			return var_26_3, var_26_1, var_26_2
		end
	end

	return nil, var_26_1, var_26_2
end

function GUI.sendCommand(arg_27_0, arg_27_1, arg_27_2)
	if arg_27_1.currentState ~= "disabled" then
		return arg_27_1:sendCommand(arg_27_2)
	end
end

function GUI.sendBubbleCommand(arg_28_0, arg_28_1, arg_28_2)
	if arg_28_1.currentState ~= "disabled" then
		while arg_28_1 ~= nil and (not arg_28_1.sendCommand or not arg_28_1:sendCommand(arg_28_2)) do
			arg_28_1 = arg_28_1:getParentNode()
		end

		if arg_28_1 then
			return arg_28_1
		end
	end

	return nil
end

function GUI.pressElement(arg_29_0, arg_29_1)
	arg_29_0:sendCommand(arg_29_1, var_0_2(GUI_COMMAND_PRESS))

	if arg_29_0.buttonMode == "push" then
		arg_29_0:sendCommand(arg_29_1, var_0_2(GUI_COMMAND_CLICK))
	end
end

function GUI.releaseElement(arg_30_0, arg_30_1)
	if arg_30_0.buttonMode == "release" then
		arg_30_0:sendCommand(arg_30_1, var_0_2(GUI_COMMAND_CLICK))
	end

	arg_30_0:sendCommand(arg_30_1, var_0_2(GUI_COMMAND_RELEASE))
end

function GUI.clickElement(arg_31_0, arg_31_1)
	arg_31_0:sendCommand(arg_31_1, var_0_2(GUI_COMMAND_CLICK))
end

function GUI.updateInputs(arg_32_0, arg_32_1)
	if arg_32_0.disabled then
		return
	end

	if GUI.orient_control and not GUI:isControlDown(GUI.orient_device, GUI.orient_control, GUI.orient_threshold) then
		GUI.orient_control_off_trigger = true
		GUI.orient_control = false
	end

	if GUI.REPEAT_ORIENT and not GUI.orient_control then
		local var_32_0 = GUI.gameDevice
		local var_32_1
		local var_32_2

		if GUI:isControlPressed(var_32_0, GAMEPAD_PAD_LEFT, GUI.KEY_THRESHOLD) then
			var_32_1 = GAMEPAD_PAD_LEFT
			var_32_2 = GUI.KEY_THRESHOLD
		elseif GUI:isControlPressed(var_32_0, GAMEPAD_PAD_RIGHT, GUI.KEY_THRESHOLD) then
			var_32_1 = GAMEPAD_PAD_RIGHT
			var_32_2 = GUI.KEY_THRESHOLD
		elseif GUI:isControlPressed(var_32_0, GAMEPAD_PAD_UP, GUI.KEY_THRESHOLD) then
			var_32_1 = GAMEPAD_PAD_UP
			var_32_2 = GUI.KEY_THRESHOLD
		elseif GUI:isControlPressed(var_32_0, GAMEPAD_PAD_DOWN, GUI.KEY_THRESHOLD) then
			var_32_1 = GAMEPAD_PAD_DOWN
			var_32_2 = GUI.KEY_THRESHOLD
		end

		if var_32_1 and var_32_2 then
			GUI.orient_device = var_32_0
			GUI.orient_control = var_32_1
			GUI.orient_threshold = var_32_2
		end
	end

	arg_32_0.dt = arg_32_1
	arg_32_0.currentTime = arg_32_0.currentTime + arg_32_1
	arg_32_0.repeatedThisFrame = false

	local var_32_3 = arg_32_0.focusedElement
	local var_32_4 = GUI:isTouched()
	local var_32_5 = GUI:wasTouched()
	local var_32_6

	var_32_6 = var_32_4 and not var_32_5

	local var_32_7

	var_32_7 = not var_32_4 and var_32_5

	if var_32_3 ~= nil then
		if GUI:isAnythingPressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Anything key pressed")
			end

			arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_KEYPRESS))
		end

		if GUI:isLeftPressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key left pressed")
			end

			if not arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_LEFT)) then
				local var_32_8 = arg_32_0:getNeighborElement(var_32_3, "GUI_DIRECTION_LEFT")

				if var_32_8 ~= nil then
					arg_32_0:focusElement(var_32_8)
				end
			end
		elseif GUI:isRightPressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key right pressed")
			end

			if not arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_RIGHT)) then
				local var_32_9 = arg_32_0:getNeighborElement(var_32_3, "GUI_DIRECTION_RIGHT")

				if var_32_9 ~= nil then
					arg_32_0:focusElement(var_32_9)
				end
			end
		elseif GUI:isUpPressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key up pressed")
			end

			if not arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_UP)) then
				local var_32_10 = arg_32_0:getNeighborElement(var_32_3, "GUI_DIRECTION_UP")

				if var_32_10 ~= nil then
					arg_32_0:focusElement(var_32_10)
				end
			end
		elseif GUI:isDownPressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key down pressed")
			end

			if not arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_DOWN)) then
				local var_32_11 = arg_32_0:getNeighborElement(var_32_3, "GUI_DIRECTION_DOWN")

				if var_32_11 ~= nil then
					arg_32_0:focusElement(var_32_11)
				end
			end
		elseif GUI:isValidatePressed() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key validate pressed")
			end

			arg_32_0.validatedTarget = var_32_3

			arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_PRESS))

			if arg_32_0.buttonMode == "push" then
				arg_32_0:sendCommand(var_32_3, var_0_2(GUI_COMMAND_CLICK))
			end
		elseif GUI:isValidateDown() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key validate down")
			end
		elseif GUI:isValidateReleased() then
			if GUI.DEBUG_LEVEL.inputs then
				GUI:debugPrint("Key validate released")
			end

			if arg_32_0.validatedTarget ~= nil then
				if arg_32_0.buttonMode == "release" then
					arg_32_0:sendCommand(arg_32_0.validatedTarget, var_0_2(GUI_COMMAND_CLICK))
				end

				arg_32_0:sendCommand(arg_32_0.validatedTarget, var_0_2(GUI_COMMAND_RELEASE))

				arg_32_0.validatedTarget = nil
			end
		end
	end

	arg_32_0.pressedCommandTarget = nil
	arg_32_0.dragCommandTarget = nil
	arg_32_0.isDragging = false
	arg_32_0.dragHasFailed = false
	GUI.orient_control_off_trigger = false
end

function GUI.focusElement(arg_33_0, arg_33_1, arg_33_2)
	if GUI.DEBUG_LEVEL.focus then
		GUI:debugPrint("Focus element " .. tostring(arg_33_1))
	end

	local var_33_0 = arg_33_0.focusedElement

	if var_33_0 ~= arg_33_1 and arg_33_0.pressedCommandTarget == nil then
		if var_33_0 ~= nil then
			if var_33_0 == arg_33_0.validatedTarget then
				arg_33_0:sendCommand(var_33_0, var_0_2(GUI_COMMAND_RELEASE))

				arg_33_0.validatedTarget = nil
			end

			arg_33_0:sendCommand(var_33_0, var_0_2(GUI_COMMAND_UNFOCUS))
		end

		if arg_33_1 ~= nil and arg_33_1.currentState ~= "disabled" then
			var_33_0 = arg_33_1
		else
			var_33_0 = arg_33_1
		end

		arg_33_0.focusedElement = var_33_0

		if var_33_0 ~= nil then
			arg_33_0:sendCommand(var_33_0, var_0_3(arg_33_2))
		end
	end
end

function GUI.enforceListener(arg_34_0, arg_34_1, arg_34_2, arg_34_3, arg_34_4)
	if arg_34_1.listener ~= nil and arg_34_1.listener ~= arg_34_2 then
		GUI:warning("Warning! the listener of the " .. tostring(arg_34_4) .. " associated to a " .. tostring(arg_34_3) .. " script does not reference the " .. tostring(arg_34_3) .. ". Value will be overriden.")
	end

	arg_34_1.listener = arg_34_2
end
