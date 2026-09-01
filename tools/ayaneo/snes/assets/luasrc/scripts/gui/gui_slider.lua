require("gui_element.lua")

GUISlider = class(GUIElement)
GUISlider.states = class(GUIElement.states)
GUISlider.states.disabled = class()

function GUISlider.states.disabled.enter(arg_1_0, arg_1_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUISlider : Enter disabled " .. tostring(arg_1_0) .. " previousState = " .. tostring(arg_1_1))
	end

	GUIElement.states.disabled.enter(arg_1_0, arg_1_1)
	arg_1_0.thumbElement:setState("disabled")
end

function GUISlider.states.disabled.leave(arg_2_0, arg_2_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUISlider : leave disabled " .. tostring(arg_2_0))
	end

	GUIElement.states.disabled.leave(arg_2_0, arg_2_1)
	arg_2_0.thumbElement:setState(arg_2_1)
end

function GUISlider.start(arg_3_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_3_0) .. "]\tGUISlider:start()")
	end

	if arg_3_0.thumbElement == nil then
		GUI:error(" GUISlider does not have a reference to a thumb element")

		return
	else
		if arg_3_0.thumbElement.idleVisual ~= nil then
			arg_3_0.thumbElementWidth, arg_3_0.thumbElementHeight = arg_3_0.thumbElement.idleVisual:getSize()
		end

		arg_3_0:enforceListenerToSelf(arg_3_0.thumbElement, "GUISlider", "Thumb")

		arg_3_0.thumbElement.preListener = arg_3_0
	end

	if arg_3_0.direction == nil then
		arg_3_0.direction = "horizontal"
	end

	if arg_3_0.startValue == nil then
		arg_3_0.startValue = 0
	end

	if arg_3_0.endValue == nil then
		arg_3_0.endValue = 1
	end

	arg_3_0:setValueBounds(arg_3_0.startValue, arg_3_0.endValue)

	if arg_3_0.dragThreshold == nil then
		arg_3_0.dragThreshold = 4
	end

	if arg_3_0.increment == nil then
		arg_3_0.increment = 0.2
	end

	if arg_3_0.states == nil then
		arg_3_0.states = GUIElement.element_states
	end

	initializeElementSound(arg_3_0.newValueSound)
	arg_3_0:setupClickableArea()
	GUIElement.start(arg_3_0)

	if arg_3_0.currentValue == nil then
		arg_3_0.currentValue = 0
	end

	arg_3_0:setValue(arg_3_0.currentValue, true)

	arg_3_0.isDragEngaged = false
	arg_3_0.isDragging = false
end

function GUISlider.onCommand(arg_4_0, arg_4_1)
	if GUI.DEBUG_LEVEL.commands then
		GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider:onCommand() " .. tostring(arg_4_1.id))
	end

	if arg_4_1.id == GUI_COMMAND_FOCUS or arg_4_1.id == GUI_COMMAND_UNFOCUS then
		return arg_4_0.thumbElement:sendCommand(arg_4_1)
	elseif arg_4_1.id == GUI_COMMAND_CLICK then
		if arg_4_1.posX and arg_4_1.posY then
			local var_4_0, var_4_1 = arg_4_0:worldToLocalPosition(arg_4_1.posX, arg_4_1.posY)

			arg_4_0:moveThumb(var_4_0, var_4_1)
		end

		return arg_4_0.thumbElement:sendCommand(arg_4_1)
	elseif arg_4_1.id == GUI_COMMAND_DRAG then
		local var_4_2, var_4_3 = arg_4_0:worldToLocalPosition(arg_4_1.posX, arg_4_1.posY)

		if arg_4_1.first then
			arg_4_0.isDragEngaged = false
		end

		if not arg_4_0.isDragging then
			if not arg_4_0.isDragEngaged then
				local var_4_4, var_4_5 = arg_4_0:worldToLocalPosition(arg_4_1.previousPosX, arg_4_1.previousPosY)

				arg_4_0.isDragEngaged = true

				local var_4_6 = math.abs(var_4_2 - var_4_4) > math.abs(var_4_3 - var_4_5)

				if arg_4_0.direction == "horizontal" and not var_4_6 or arg_4_0.direction == "vertical" and var_4_6 then
					return false
				end

				arg_4_0.isDragging = true

				if arg_4_0.listener ~= nil and arg_4_0.listener.onSliderDragStart ~= nil then
					arg_4_0.listener:onSliderDragStart(arg_4_0, arg_4_0.currentValue)
				end

				return true
			end
		else
			arg_4_0:moveThumb(var_4_2, var_4_3)

			return true
		end
	elseif arg_4_1.id == GUI_COMMAND_RELEASE then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider release ")
		end

		if arg_4_0.listener ~= nil and arg_4_0.listener.onSliderReleased ~= nil then
			arg_4_0.listener:onSliderReleased(arg_4_0, arg_4_0.currentValue)
		end

		arg_4_0.isDragEngaged = false
		arg_4_0.isDragging = false

		arg_4_0:setValue(arg_4_0.currentValue, true)

		return arg_4_0.thumbElement:sendCommand(arg_4_1)
	elseif arg_4_1.id == GUI_COMMAND_LEFT and arg_4_0.direction == "horizontal" and arg_4_0.currentValue > arg_4_0.startValue then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider Key left " .. tostring(arg_4_0))
		end

		arg_4_0:setValue(arg_4_0.currentValue - arg_4_0.increment)

		return true
	elseif arg_4_1.id == GUI_COMMAND_RIGHT and arg_4_0.direction == "horizontal" and arg_4_0.currentValue < arg_4_0.endValue then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider Key right " .. tostring(arg_4_0))
		end

		arg_4_0:setValue(arg_4_0.currentValue + arg_4_0.increment)

		return true
	elseif arg_4_1.id == GUI_COMMAND_UP and arg_4_0.direction == "vertical" and arg_4_0.currentValue > arg_4_0.startValue then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider Key up " .. tostring(arg_4_0))
		end

		arg_4_0:setValue(arg_4_0.currentValue - arg_4_0.increment)

		return true
	elseif arg_4_1.id == GUI_COMMAND_DOWN and arg_4_0.direction == "vertical" and arg_4_0.currentValue < arg_4_0.endValue then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUISlider Key down " .. tostring(arg_4_0))
		end

		arg_4_0:setValue(arg_4_0.currentValue + arg_4_0.increment)

		return true
	else
		return GUIElement.onCommand(arg_4_0, arg_4_1)
	end
end

function GUISlider.onButtonPress(arg_5_0, arg_5_1, arg_5_2, arg_5_3)
	if not arg_5_2 or not arg_5_3 then
		return
	end

	if arg_5_1 == arg_5_0.thumbElement then
		if GUI.DEBUG_LEVEL.callbacks then
			GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUISlider -> thumb pressed @ " .. arg_5_2 .. "," .. arg_5_3)
		end

		local var_5_0, var_5_1 = arg_5_0.thumbElement:getLocalPosition()

		sliderPosX = var_5_0 + arg_5_2
		sliderPosY = var_5_1 + arg_5_3

		if not arg_5_0.thumbPressedStarted then
			arg_5_0.thumbPressedStarted = true
			arg_5_0.thumbPressedStartPosX = sliderPosX
			arg_5_0.thumbPressedStartPosY = sliderPosY

			if arg_5_0.listener ~= nil and arg_5_0.listener.onSliderPressed ~= nil then
				arg_5_0.listener:onSliderPressed(arg_5_0, arg_5_0.currentValue)
			end
		elseif not arg_5_0.thumbDragStarted then
			if arg_5_0.direction == "horizontal" and math.abs(arg_5_0.thumbPressedStartPosX - sliderPosX) > arg_5_0.dragThreshold then
				arg_5_0.thumbDragStarted = true
			elseif arg_5_0.direction == "vertical" and math.abs(arg_5_0.thumbPressedStartPosY, sliderPosY) > arg_5_0.dragThreshold then
				arg_5_0.thumbDragStarted = true
			end
		else
			arg_5_0:moveThumb(sliderPosX, sliderPosY)
		end
	end
end

function GUISlider.onButtonRelease(arg_6_0, arg_6_1)
	if arg_6_1 == arg_6_0.thumbElement then
		if GUI.DEBUG_LEVEL.callbacks then
			GUI:debugPrint("[" .. tostring(arg_6_0) .. "]\tGUISlider -> Thumb released")
		end

		arg_6_0.thumbPressedStarted = false
		arg_6_0.thumbDragStarted = false
	end
end

function GUISlider.setValueBounds(arg_7_0, arg_7_1, arg_7_2)
	arg_7_0.startValue = arg_7_1
	arg_7_0.endValue = arg_7_2
	arg_7_0.minValue = math.min(arg_7_0.startValue, arg_7_0.endValue)
	arg_7_0.maxValue = math.max(arg_7_0.startValue, arg_7_0.endValue)
end

function GUISlider.getNormalizedValue(arg_8_0)
	return (arg_8_0.currentValue - arg_8_0.startValue) / (arg_8_0.endValue - arg_8_0.startValue)
end

function GUISlider.setNormalizedValue(arg_9_0, arg_9_1)
	arg_9_0:setValue(arg_9_0.startValue * (1 - arg_9_1) + arg_9_0.endValue * arg_9_1)
end

function GUISlider.setValue(arg_10_0, arg_10_1, arg_10_2)
	if arg_10_0:internalSetValue(arg_10_1) or arg_10_2 then
		local var_10_0 = arg_10_0:getNormalizedValue()
		local var_10_1, var_10_2 = arg_10_0.clickable:getBoxShapeSize()

		if arg_10_0.direction == "horizontal" then
			local var_10_3 = var_10_1 * var_10_0 - var_10_1 / 2

			arg_10_0.thumbElement:setLocalPosition(var_10_3, 0)
		elseif arg_10_0.direction == "vertical" then
			local var_10_4 = var_10_2 * var_10_0 - var_10_2 / 2

			arg_10_0.thumbElement:setLocalPosition(0, var_10_4)
		end

		arg_10_0:onValueChanged()
	end
end

function GUISlider.internalSetValue(arg_11_0, arg_11_1)
	local var_11_0 = arg_11_0.currentValue

	arg_11_0.currentValue = Math.clamp(math.floor((arg_11_1 + arg_11_0.increment / 2) / arg_11_0.increment) * arg_11_0.increment, arg_11_0.minValue, arg_11_0.maxValue)

	if arg_11_0.label then
		arg_11_0.label:setText(tostring(arg_11_0.currentValue))
	end

	return arg_11_0.currentValue ~= var_11_0
end

function GUISlider.moveThumb(arg_12_0, arg_12_1, arg_12_2)
	local var_12_0
	local var_12_1, var_12_2 = arg_12_0.clickable:getBoxShapeSize()

	if arg_12_0.direction == "horizontal" then
		arg_12_1 = Math.clamp(arg_12_1, -var_12_1 / 2, var_12_1 / 2)

		arg_12_0.thumbElement:setLocalPosition(arg_12_1, 0)

		local var_12_3 = (arg_12_1 + var_12_1 / 2) / var_12_1

		if arg_12_0:internalSetValue(arg_12_0.startValue * (1 - var_12_3) + arg_12_0.endValue * var_12_3) then
			arg_12_0:onValueChanged()
		end
	elseif arg_12_0.direction == "vertical" then
		arg_12_2 = Math.clamp(arg_12_2, -var_12_2 / 2, var_12_2 / 2)

		arg_12_0.thumbElement:setLocalPosition(0, arg_12_2)

		local var_12_4 = (arg_12_2 + var_12_2 / 2) / var_12_2

		if arg_12_0:internalSetValue(arg_12_0.startValue * (1 - var_12_4) + arg_12_0.endValue * var_12_4) then
			arg_12_0:onValueChanged()
		end
	end
end

function GUISlider.onValueChanged(arg_13_0)
	if arg_13_0.listener ~= nil and arg_13_0.listener.onSliderValueChange ~= nil then
		arg_13_0.listener:onSliderValueChange(arg_13_0, arg_13_0.currentValue)
	end

	if arg_13_0.newValueSound then
		arg_13_0.newValueSound:play()
	end
end
