require("gui_button.lua")

GUICheckButton = class(GUIButton)
GUICheckButton.states = class(GUIButton.states)
GUICheckButton.states.idle = class(GUIButton.states.idle)

function GUICheckButton.states.idle.enter(arg_1_0, arg_1_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUICheckButton : enter idle " .. tostring(arg_1_0))
	end

	arg_1_0:updateVisual(false)
end

function GUICheckButton.states.idle.leave(arg_2_0, arg_2_1)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUICheckButton : leave idle " .. tostring(arg_2_0))
	end

	arg_2_0:disableVisual(arg_2_0.checkedVisual)
	arg_2_0:disableVisual(arg_2_0.uncheckedVisual)
end

GUICheckButton.states.pressed = class(GUIButton.states.pressed)

function GUICheckButton.states.pressed.enter(arg_3_0, arg_3_1, ...)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUICheckButton : enter pressed " .. tostring(arg_3_0))
	end

	arg_3_0:updateVisual(true)

	if select(1, ...) and arg_3_0.enteredSound then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play entered sound")
		end

		arg_3_0.enteredSound:stop()
		arg_3_0.enteredSound:play()
	elseif arg_3_0.pressedSound ~= nil then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play pressed sound")
		end

		arg_3_0.pressedSound:stop()
		arg_3_0.pressedSound:play()
	end
end

function GUICheckButton.states.pressed.leave(arg_4_0, arg_4_1, ...)
	if GUI.DEBUG_LEVEL.states then
		GUI:debugPrint("GUICheckButton : leave pressed " .. tostring(arg_4_0))
	end

	arg_4_0:disableVisual(arg_4_0.checkedPressVisual)
	arg_4_0:disableVisual(arg_4_0.uncheckedPressVisual)

	if select(1, ...) and arg_4_0.exitedSound then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play exited sound")
		end

		arg_4_0.exitedSound:stop()
		arg_4_0.exitedSound:play()
	elseif arg_4_0.releasedSound ~= nil then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play released sound")
		end

		arg_4_0.releasedSound:stop()
		arg_4_0.releasedSound:play()
	end
end

function GUICheckButton.start(arg_5_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUICheckButton:start()")
	end

	if not arg_5_0.uncheckedVisual then
		arg_5_0.uncheckedVisual = arg_5_0.idleVisual
	end

	if not arg_5_0.uncheckedPressVisual then
		arg_5_0.uncheckedPressVisual = arg_5_0.pressedVisual
	end

	if not arg_5_0.uncheckedPressVisual then
		arg_5_0.uncheckedPressVisual = arg_5_0.idleVisual
	end

	if not arg_5_0.checkedPressVisual then
		arg_5_0.checkedPressVisual = arg_5_0.checkedVisual
	end

	arg_5_0:disableVisual(arg_5_0.checkedPressVisual)
	arg_5_0:disableVisual(arg_5_0.uncheckedPressVisual)
	arg_5_0:disableVisual(arg_5_0.checkedVisual)
	arg_5_0:disableVisual(arg_5_0.uncheckedVisual)
	initializeElementSound(arg_5_0.checkedSound)
	initializeElementSound(arg_5_0.uncheckedSound)
	GUIButton.start(arg_5_0)

	if arg_5_0.checked == nil then
		arg_5_0:setChecked(false)
	else
		arg_5_0:setChecked(arg_5_0.checked)
	end
end

function GUICheckButton.updateVisual(arg_6_0, arg_6_1)
	if arg_6_1 == nil then
		arg_6_1 = arg_6_0.currentState == "pressed"
	end

	if not arg_6_1 then
		if not arg_6_0.checked then
			arg_6_0:disableVisual(arg_6_0.checkedVisual)
			arg_6_0:enableVisual(arg_6_0.uncheckedVisual)
		else
			arg_6_0:disableVisual(arg_6_0.uncheckedVisual)
			arg_6_0:enableVisual(arg_6_0.checkedVisual)
		end
	elseif not arg_6_0.checked then
		arg_6_0:disableVisual(arg_6_0.checkedPressVisual)
		arg_6_0:enableVisual(arg_6_0.uncheckedPressVisual)
	else
		arg_6_0:disableVisual(arg_6_0.uncheckedPressVisual)
		arg_6_0:enableVisual(arg_6_0.checkedPressVisual)
	end
end

function GUICheckButton.playClickedEffects(arg_7_0)
	if arg_7_0.checked then
		if not arg_7_0:playCheckedSound() then
			arg_7_0:playClickedSound()
		end

		if not arg_7_0:playCheckedAnimation() then
			arg_7_0:playClickedAnimation()
		end
	else
		if not arg_7_0:playUncheckedSound() then
			arg_7_0:playClickedSound()
		end

		if not arg_7_0:playUncheckedAnimation() then
			arg_7_0:playClickedAnimation()
		end
	end
end

function GUICheckButton.playCheckedSound(arg_8_0)
	if arg_8_0.checkedSound ~= nil then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play checked sound")
		end

		arg_8_0.checkedSound:stop()
		arg_8_0.checkedSound:play()

		return true
	end
end

function GUICheckButton.playUncheckedSound(arg_9_0)
	if arg_9_0.uncheckedSound ~= nil then
		if GUI.DEBUG_LEVEL.sounds then
			GUI:debugPrint("GUICheckButton : play unchecked sound")
		end

		arg_9_0.uncheckedSound:stop()
		arg_9_0.uncheckedSound:play()

		return true
	end
end

function GUICheckButton.playCheckedAnimation(arg_10_0)
	if arg_10_0.checkedAnimation ~= nil then
		if GUI.DEBUG_LEVEL.animations then
			GUI:debugPrint("GUICheckButton : play checked animation")
		end

		arg_10_0:playAnimation(arg_10_0.checkedAnimation)

		return true
	end
end

function GUICheckButton.playUncheckedAnimation(arg_11_0)
	if arg_11_0.uncheckedAnimation ~= nil then
		if GUI.DEBUG_LEVEL.animations then
			GUI:debugPrint("GUICheckButton : play unchecked animation")
		end

		arg_11_0:playAnimation(arg_11_0.uncheckedAnimation)

		return true
	end
end

function GUICheckButton.toggle(arg_12_0)
	if not arg_12_0.radioMode or not arg_12_0.checked then
		arg_12_0:setChecked(not arg_12_0.checked)
		arg_12_0:updateVisual()
		arg_12_0:playClickedEffects()

		if arg_12_0.listener ~= nil and arg_12_0.listener.onCheckToggle ~= nil then
			arg_12_0.listener:onCheckToggle(arg_12_0, arg_12_0.checked)
		end
	end
end

function GUICheckButton.onCommand(arg_13_0, arg_13_1)
	if GUI.DEBUG_LEVEL.commands then
		GUI:debugPrint("[" .. tostring(arg_13_0) .. "]\tGUICheckButton:onCommand() " .. tostring(arg_13_1.id))
	end

	if arg_13_1.id == GUI_COMMAND_CLICK then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_13_0) .. "]\tGUICheckButton clicked ")
		end

		arg_13_0:toggle()

		return true
	else
		return GUIButton.onCommand(arg_13_0, arg_13_1)
	end
end

function GUICheckButton.setChecked(arg_14_0, arg_14_1)
	if arg_14_0.checked == arg_14_1 then
		return
	end

	arg_14_0.checked = arg_14_1

	if not arg_14_0.checked then
		arg_14_0:disableVisual(arg_14_0.checkedVisual)
		arg_14_0:enableVisual(arg_14_0.uncheckedVisual)
	else
		arg_14_0:disableVisual(arg_14_0.uncheckedVisual)
		arg_14_0:enableVisual(arg_14_0.checkedVisual)
	end
end

function GUICheckButton.isChecked(arg_15_0)
	return arg_15_0.checked
end
