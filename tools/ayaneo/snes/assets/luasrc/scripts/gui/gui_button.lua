require("gui_element.lua")

GUIButton = class(GUIElement)
GUIButton.states = class(GUIElement.states)
GUIButton.states.pressed = class()

function GUIButton.states.pressed.enter(arg_1_0, arg_1_1, ...)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIButton : enter pressed " .. tostring(arg_1_0))
	end

	if arg_1_0.pressedVisual ~= nil then
		arg_1_0:enableVisual(arg_1_0.pressedVisual)
	else
		arg_1_0:enableVisual(arg_1_0.idleVisual)
	end

	if arg_1_0.pressedAnimation ~= nil then
		if GUI.DEBUG_LEVEL.animations then
			GUI:debugPrint("GUIButton : play pressed animation")
		end

		arg_1_0.pressedAnimation:enable()
		arg_1_0.pressedAnimation:setSpeed(1)
		arg_1_0.pressedAnimation:play()
	end

	if select(1, ...) and arg_1_0.enteredSound then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUIButton : play entered sound")
		end

		arg_1_0.enteredSound:stop()
		arg_1_0.enteredSound:play()
	elseif arg_1_0.pressedSound ~= nil then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUIButton : play pressed sound")
		end

		arg_1_0.pressedSound:stop()
		arg_1_0.pressedSound:play()
	end
end

function GUIButton.states.pressed.leave(arg_2_0, arg_2_1, ...)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUIButton : leave pressed " .. tostring(arg_2_0))
	end

	if arg_2_0.pressedVisual ~= nil then
		arg_2_0:disableVisual(arg_2_0.pressedVisual)
	else
		arg_2_0:disableVisual(arg_2_0.idleVisual)
	end

	if arg_2_0.pressedAnimation ~= nil then
		if GUI.DEBUG_LEVEL.animations then
			GUI:debugPrint("GUIButton : play released animation")
		end

		arg_2_0.pressedAnimation:setSpeed(-1)
		arg_2_0.pressedAnimation:play()
	end

	if select(1, ...) and arg_2_0.exitedSound then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUIButton : play exited sound")
		end

		arg_2_0.exitedSound:stop()
		arg_2_0.exitedSound:play()
	elseif arg_2_0.releasedSound ~= nil and (arg_2_0.clickedSound == nil or not arg_2_0.clickedSound:isPlaying()) then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUIButton : play released sound")
		end

		arg_2_0.releasedSound:stop()
		arg_2_0.releasedSound:play()
	end
end

function GUIButton.start(arg_3_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_3_0) .. "]\tGUIButton:start()")
	end

	arg_3_0:setupClickableArea()
	arg_3_0:disableVisual(arg_3_0.pressedVisual)
	arg_3_0:setupAnimator(arg_3_0.clickedAnimation)
	arg_3_0:setupAnimator(arg_3_0.pressedAnimation)
	arg_3_0:setupAnimator(arg_3_0.releasedAnimation)
	initializeElementSound(arg_3_0.pressedSound)
	initializeElementSound(arg_3_0.enteredSound)
	initializeElementSound(arg_3_0.clickedSound)
	initializeElementSound(arg_3_0.exitedSound)
	initializeElementSound(arg_3_0.releasedSound)
	GUIElement.start(arg_3_0)
	GUI:registerButton(arg_3_0)
end

function GUIButton.stop(arg_4_0)
	GUI:unregisterButton(arg_4_0)
	GUIElement.stop(arg_4_0)
end

function GUIButton.playClickedEffects(arg_5_0)
	arg_5_0:playClickedAnimation()
	arg_5_0:playClickedSound()
end

function GUIButton.playClickedAnimation(arg_6_0)
	if arg_6_0.clickedAnimation ~= nil then
		if GUI.DEBUG_LEVEL.animations then
			GUI:debugPrint("GUIButton : play clicked animation")
		end

		arg_6_0:playAnimation(arg_6_0.clickedAnimation)
	end
end

function GUIButton.playClickedSound(arg_7_0)
	if arg_7_0.clickedSound then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUIButton : play clicked sound")
		end

		arg_7_0.clickedSound:stop()
		arg_7_0.clickedSound:play()
	end
end

function GUIButton.resetAnimations(arg_8_0)
	if arg_8_0.clickedAnimation then
		arg_8_0:resetAnimation(arg_8_0.clickedAnimation)
	end

	if arg_8_0.pressedAnimation then
		arg_8_0:resetAnimation(arg_8_0.pressedAnimation)
	end

	if arg_8_0.releasedAnimation then
		arg_8_0:resetAnimation(arg_8_0.releasedAnimation)
	end
end

function GUIButton.press(arg_9_0, arg_9_1, arg_9_2, arg_9_3)
	arg_9_0:setState("pressed", arg_9_3)

	if arg_9_0.listener ~= nil and arg_9_0.listener.onButtonPress ~= nil then
		arg_9_0.listener:onButtonPress(arg_9_0, arg_9_1, arg_9_2)
	end
end

function GUIButton.release(arg_10_0, arg_10_1, arg_10_2, arg_10_3)
	arg_10_0:setState("idle", arg_10_3)

	if arg_10_0.listener ~= nil and arg_10_0.listener.onButtonRelease ~= nil then
		arg_10_0.listener:onButtonRelease(arg_10_0, arg_10_1, arg_10_2)
	end
end

function GUIButton.click(arg_11_0)
	arg_11_0:playClickedEffects()

	if arg_11_0.listener ~= nil and arg_11_0.listener.onButtonClick ~= nil then
		arg_11_0.listener:onButtonClick(arg_11_0)
	end
end

function GUIButton.onCommand(arg_12_0, arg_12_1)
	if GUI.DEBUG_LEVEL.commands then
		GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIButton:onCommand() " .. tostring(arg_12_1.id))
	end

	if arg_12_1.id == GUI_COMMAND_PRESS then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIButton pressed ")
		end

		arg_12_0:press(arg_12_1.posX, arg_12_1.posY, arg_12_1.overstep)

		return true
	elseif arg_12_1.id == GUI_COMMAND_RELEASE then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIButton released ")
		end

		arg_12_0:release(arg_12_1.posX, arg_12_1.posY, arg_12_1.overstep)

		return true
	elseif arg_12_1.id == GUI_COMMAND_CLICK then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIButton clicked ")
		end

		arg_12_0:click()

		return true
	else
		return GUIElement.onCommand(arg_12_0, arg_12_1)
	end
end
