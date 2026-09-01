require("/scripts/core/core.lua")

sys_dialog = class(gui_container)

function sys_dialog.start(arg_1_0)
	gui_container.start(arg_1_0)
	table.insert(system.dialogs, arg_1_0)

	if not arg_1_0.btn_yes then
		arg_1_0.btn_yes = arg_1_0.elements:getChildByName("btn_decide")
	end

	if not arg_1_0.btn_no then
		arg_1_0.btn_no = arg_1_0.elements:getChildByName("btn_cancel")
	end

	arg_1_0.org_focus = arg_1_0.focus
end

function sys_dialog.showDialog(arg_2_0, arg_2_1, arg_2_2, arg_2_3, arg_2_4)
	arg_2_0.invoker = arg_2_1 or GUI.focusedElement
	arg_2_0.func = arg_2_2
	arg_2_0.func_no = arg_2_3
	arg_2_0.not_close = arg_2_4
	arg_2_0.current = arg_2_0.focus

	arg_2_0:enable()
	arg_2_0:activate()
end

function sys_dialog.closeDialog(arg_3_0)
	if arg_3_0:isFocused() or arg_3_0:elementIsFocused() then
		GUI:focusElement(arg_3_0.invoker)
	end

	arg_3_0:deactivate()
end

function sys_dialog.onButtonClick(arg_4_0, arg_4_1)
	local var_4_0 = arg_4_0.elementArray
	local var_4_1 = table_find(var_4_0, arg_4_1)

	if arg_4_1.type == "no" then
		if arg_4_0.func_no then
			arg_4_0.func_no()
		end
	elseif arg_4_1.type == "yes" and arg_4_0.func then
		arg_4_0.func()
	end

	if not arg_4_0.not_close then
		if arg_4_0:isFocused() or arg_4_0:elementIsFocused() then
			GUI:focusElement(arg_4_0.invoker)
		end

		arg_4_0:deactivate()
	end
end

function sys_dialog.onElementCommand(arg_5_0, arg_5_1, arg_5_2)
	return gui_container.onElementCommand(arg_5_0, arg_5_1, arg_5_2)
end

function sys_dialog.onContainerCanceled(arg_6_0, arg_6_1)
	local var_6_0 = arg_6_0.elementArray

	if table_find_if(var_6_0, function(arg_7_0)
		return arg_7_0.type == "no"
	end) then
		if arg_6_0.func_no then
			arg_6_0.func_no()
		end

		if not arg_6_0.not_close then
			if arg_6_0:isFocused() or arg_6_0:elementIsFocused() then
				GUI:focusElement(arg_6_0.invoker)
			end

			arg_6_0:deactivate()

			if arg_6_0.cancelSound then
				arg_6_0.cancelSound:stop()
				arg_6_0.cancelSound:play()
			end
		end
	end
end

function sys_dialog.setText(arg_8_0, arg_8_1)
	arg_8_0.textLabel:setText(arg_8_1)
end

function sys_dialog.setFocusYes(arg_9_0)
	arg_9_0.focus = arg_9_0.btn_yes
end

function sys_dialog.setFocusNo(arg_10_0)
	arg_10_0.focus = arg_10_0.btn_no
end

function sys_dialog.resetFocus(arg_11_0)
	arg_11_0.focus = arg_11_0.org_focus
end

function sys_dialog.setButtonText(arg_12_0, arg_12_1, arg_12_2)
	if arg_12_1 then
		arg_12_0.btn_yes:setText(arg_12_1)
	end

	if arg_12_2 then
		arg_12_0.btn_no:setText(arg_12_2)
	end
end

function sys_dialog.isAnimation(arg_13_0)
	if arg_13_0.activeAnimation and arg_13_0.activeAnimation:isRunning() then
		return true
	end

	if arg_13_0.deactiveAnimation and arg_13_0.deactiveAnimation:isRunning() then
		return true
	end

	return false
end

function sys_dialog.isClosed(arg_14_0)
	if arg_14_0.isActive then
		return false
	end

	if arg_14_0:isAnimation() then
		return false
	end

	return true
end
