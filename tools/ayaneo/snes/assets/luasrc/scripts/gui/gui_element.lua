require("../core/core.lua")

GUIElement = class(WorldNode)
GUIElement.states = class()
GUIElement.isGUIElement = true
GUIElement.states.idle = class()

function GUIElement.states.idle.enter(arg_1_0, arg_1_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIElement : Enter idle " .. tostring(arg_1_0) .. " previousState = " .. tostring(arg_1_1))
	end

	arg_1_0:enableVisual(arg_1_0.idleVisual)
end

function GUIElement.states.idle.leave(arg_2_0, arg_2_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIElement : leave idle " .. tostring(arg_2_0))
	end

	arg_2_0:disableVisual(arg_2_0.idleVisual)
end

GUIElement.states.disabled = class()

function GUIElement.states.disabled.enter(arg_3_0, arg_3_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIElement : Enter disabled " .. tostring(arg_3_0) .. " previousState = " .. tostring(arg_3_1))
	end

	if arg_3_0.disabledVisual then
		arg_3_0:enableVisual(arg_3_0.disabledVisual)
	else
		arg_3_0:enableVisual(arg_3_0.idleVisual)
	end

	arg_3_0.clickable:disable()
end

function GUIElement.states.disabled.leave(arg_4_0, arg_4_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIElement : leave disabled " .. tostring(arg_4_0))
	end

	if arg_4_0.disabledVisual then
		arg_4_0:disableVisual(arg_4_0.disabledVisual)
	else
		arg_4_0:disableVisual(arg_4_0.idleVisual)
	end

	arg_4_0.clickable:enable()
end

function GUIElement.start(arg_5_0)
	arg_5_0.currentState = "none"

	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIElement:start()")
	end

	arg_5_0:disableVisual(arg_5_0.idleVisual)
	arg_5_0:disableVisual(arg_5_0.disabledVisual)

	if arg_5_0.focusedVisual ~= nil then
		arg_5_0.focusedVisual:disable()
	end

	if arg_5_0.ownClickable == nil then
		arg_5_0.ownClickable = false
	end

	if not arg_5_0.autoFocusDisabled then
		arg_5_0.autoFocusDisabled = false
	end

	arg_5_0:setupAnimator(arg_5_0.focusedAnimation)
	arg_5_0:setState("idle")
end

function GUIElement.stop(arg_6_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_6_0) .. "]\tGUIElement:stop()")
	end

	if arg_6_0.clickable ~= nil and arg_6_0.ownClickable then
		removeAndDestroyComponentFromNode(arg_6_0, arg_6_0.clickable)

		arg_6_0.clickable = nil
	end
end

function GUIElement.update(arg_7_0, arg_7_1)
	local var_7_0 = arg_7_0.states

	if var_7_0 ~= nil then
		local var_7_1 = var_7_0[arg_7_0.currentState]

		if var_7_1 ~= nil and var_7_1.update ~= nil then
			var_7_1:update(arg_7_0, arg_7_1)
		end
	end
end

function GUIElement.setState(arg_8_0, arg_8_1, ...)
	if arg_8_1 == arg_8_0.currentState then
		return
	end

	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("[" .. tostring(arg_8_0) .. "]\tGUIElement:setState() setState = " .. arg_8_1)
	end

	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("[" .. tostring(arg_8_0) .. "]\tGUIElement:setState() curState = " .. arg_8_0.currentState)
	end

	if arg_8_0.states[arg_8_0.currentState] ~= nil and arg_8_0.states[arg_8_0.currentState].leave ~= nil then
		arg_8_0.states[arg_8_0.currentState].leave(arg_8_0, arg_8_1, ...)
	end

	local var_8_0 = arg_8_0.currentState

	arg_8_0.currentState = arg_8_1

	if arg_8_0.states[arg_8_0.currentState] ~= nil and arg_8_0.states[arg_8_0.currentState].enter ~= nil then
		arg_8_0.states[arg_8_0.currentState].enter(arg_8_0, var_8_0, ...)
	end
end

function GUIElement.setupClickableArea(arg_9_0)
	if arg_9_0.clickable == nil then
		if GUI.DEBUG_LEVEL.hit_test then
			GUI:debugPrint("[" .. tostring(arg_9_0) .. "]\tGUIElement:setupClickableArea()")
		end

		arg_9_0.clickable = addNewComponentToNode(arg_9_0, COMPONENT_TYPE_CLICKABLE)
		arg_9_0.ownClickable = true

		arg_9_0.clickable:setClickListener(arg_9_0)

		if arg_9_0.autoAdjustClickableArea == nil then
			arg_9_0.autoAdjustClickableArea = true
		end
	else
		local var_9_0 = arg_9_0.clickable:getClickListener()

		if var_9_0 ~= nil and var_9_0 ~= arg_9_0 then
			GUI:warning("Warning! the listener of a clickable associated to a GUIButton script does not reference the GUIButton. Overriding...")
		end

		arg_9_0.clickable:setClickListener(arg_9_0)

		if arg_9_0.autoAdjustClickableArea == nil then
			arg_9_0.autoAdjustClickableArea = false
		end

		arg_9_0.ownClickable = false
	end

	arg_9_0.clickable:setEnabled(arg_9_0:isSelfEnabled())
end

function GUIElement.setupAnimator(arg_10_0, arg_10_1)
	if arg_10_1 ~= nil then
		arg_10_1:disable()

		local var_10_0 = arg_10_1:getEventListener()

		if var_10_0 ~= nil and var_10_0 ~= arg_10_0 then
			GUI:warning("Warning! the event listener of an animator associated to a GUIElement script does not reference the GUIElement. Overriding...")
		end

		arg_10_1:setEventListener(arg_10_0)
	end
end

function GUIElement.enforceListenerToSelf(arg_11_0, arg_11_1, arg_11_2, arg_11_3)
	GUI:enforceListener(arg_11_1, arg_11_0, arg_11_2, arg_11_3)
end

function GUIElement.playAnimation(arg_12_0, arg_12_1)
	if arg_12_1 ~= nil then
		arg_12_1:enable()
		arg_12_1:reset()
		arg_12_1:play()
	end
end

function GUIElement.resetAnimation(arg_13_0, arg_13_1, arg_13_2)
	local var_13_0

	var_13_0 = arg_13_2 or 1

	arg_13_1:stop()
	arg_13_1:setSpeed(arg_13_2 or 1)
	arg_13_1:reset()
end

function GUIElement.enableVisual(arg_14_0, arg_14_1)
	if arg_14_1 ~= nil then
		arg_14_1:enable()

		arg_14_0.currentVisual = arg_14_1

		if arg_14_0.clickable ~= nil then
			local var_14_0 = arg_14_1:getLayer()

			arg_14_0.clickable:setLayer(var_14_0)

			local var_14_1, var_14_2 = arg_14_0.clickable:getBoxShapeSize()

			if arg_14_0.autoAdjustClickableArea or var_14_1 == 0 or var_14_2 == 0 then
				local var_14_3, var_14_4 = arg_14_1:getSize()

				if GUI.DEBUG_LEVEL.hit_test then
					GUI:debugPrint("[" .. tostring(arg_14_0) .. "]\tGUIElement clickableArea size = " .. tostring(var_14_3) .. " " .. tostring(var_14_4))
				end

				arg_14_0.clickable:setBoxShape(var_14_3, var_14_4)
			end
		end
	end
end

function GUIElement.disableVisual(arg_15_0, arg_15_1)
	if arg_15_1 ~= nil then
		arg_15_1:disable()

		arg_15_0.currentVisual = nil
	end
end

function GUIElement.isAutoFocusDisabled(arg_16_0)
	return arg_16_0.autoFocusDisabled == true
end

function GUIElement.forwardCommandToPreListener(arg_17_0, arg_17_1)
	if arg_17_0.preListener ~= nil and arg_17_0.preListener.onElementCommand ~= nil then
		return arg_17_0.preListener:onElementCommand(arg_17_0, arg_17_1)
	end

	return false
end

function GUIElement.onElementCommand(arg_18_0, arg_18_1, arg_18_2)
	return arg_18_0:forwardCommandToPreListener(arg_18_2)
end

function GUIElement.sendCommand(arg_19_0, arg_19_1)
	if arg_19_0:forwardCommandToPreListener(arg_19_1) then
		return true
	end

	return arg_19_0:onCommand(arg_19_1)
end

function GUIElement.onCommand(arg_20_0, arg_20_1)
	if GUI.DEBUG_LEVEL.commands then
		GUI:debugPrint("[" .. tostring(arg_20_0) .. "]\tGUIElement:onCommand " .. tostring(arg_20_1.id))
	end

	if arg_20_1.id == GUI_COMMAND_FOCUS then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_20_0) .. "]\tGUIElement focused " .. tostring(arg_20_0))
		end

		if arg_20_0.focusedVisual ~= nil then
			arg_20_0.focusedVisual:enable()
		end

		if arg_20_0.listener ~= nil and arg_20_0.listener.onElementFocus ~= nil then
			arg_20_0.listener:onElementFocus(arg_20_0)
		end

		if arg_20_0.focusedAnimation ~= nil then
			if GUI.DEBUG_LEVEL.animations then
				GUI:debugPrint("GUIElement : play focused animation")
			end

			arg_20_0.focusedAnimation:enable()
			arg_20_0.focusedAnimation:setSpeed(1)
			arg_20_0.focusedAnimation:play()
		end

		if arg_20_0.focusedSound ~= nil and not arg_20_1.nosound then
			if GUI.DEBUG_LEVEL.sounds then
				GUI:debugPrint("GUIElement : play focused sound")
			end

			arg_20_0.focusedSound:stop()
			arg_20_0.focusedSound:play()
		end
	elseif arg_20_1.id == GUI_COMMAND_UNFOCUS then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_20_0) .. "]\tGUIElement unfocused " .. tostring(arg_20_0))
		end

		if arg_20_0.focusedVisual ~= nil then
			arg_20_0.focusedVisual:disable()
		end

		if arg_20_0.listener ~= nil and arg_20_0.listener.onElementUnfocus ~= nil then
			arg_20_0.listener:onElementUnfocus(arg_20_0)
		end

		if arg_20_0.focusedAnimation ~= nil then
			if GUI.DEBUG_LEVEL.animations then
				GUI:debugPrint("GUIElement : play unfocused animation")
			end

			arg_20_0.focusedAnimation:setSpeed(-1)
			arg_20_0.focusedAnimation:play()
		end

		if arg_20_0.unfocusedSound ~= nil then
			if GUI.DEBUG_LEVEL.sounds then
				GUI:debugPrint("GUIElement : play unfocused sound")
			end

			arg_20_0.unfocusedSound:stop()
			arg_20_0.unfocusedSound:play()
		end
	end

	return false
end

function GUIElement.getSize(arg_21_0)
	if arg_21_0.currentVisual then
		return arg_21_0.currentVisual:getSize()
	end

	return 0, 0
end

function GUIElement.getPivot(arg_22_0)
	if arg_22_0.currentVisual then
		if arg_22_0.currentVisual:isInstanceOf(SpriteComponent) then
			return arg_22_0.currentVisual:getPivot()
		elseif arg_22_0.currentVisual:isInstanceOf(TextureComponent) then
			return arg_22_0.currentVisual:getPivot()
		elseif arg_22_0.currentVisual:isInstanceOf(LabelComponent) then
			return arg_22_0.currentVisual:getPivot()
		end
	end

	return 0, 0
end
