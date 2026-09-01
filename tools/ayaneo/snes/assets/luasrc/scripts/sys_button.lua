require("/scripts/core/core.lua")

sys_button = class(GUIButton)

function sys_button.start(arg_1_0)
	GUIButton.start(arg_1_0)

	if arg_1_0.focused_node then
		arg_1_0.focused_node:disable()
	end

	if arg_1_0.unfocused_node then
		arg_1_0.unfocused_node:enable()
	end
end

function sys_button.onCommand(arg_2_0, arg_2_1)
	GUIButton.onCommand(arg_2_0, arg_2_1)

	if arg_2_1.id == GUI_COMMAND_FOCUS then
		arg_2_0:activate()
	elseif arg_2_1.id == GUI_COMMAND_UNFOCUS then
		arg_2_0:deactivate()
	end
end

function sys_button.activate(arg_3_0)
	if arg_3_0.focused_node then
		arg_3_0.focused_node:enable()
	end

	if arg_3_0.unfocused_node then
		arg_3_0.unfocused_node:disable()
	end
end

function sys_button.deactivate(arg_4_0)
	if arg_4_0.focused_node then
		arg_4_0.focused_node:disable()
	end

	if arg_4_0.unfocused_node then
		arg_4_0.unfocused_node:enable()
	end
end

function sys_button.setText(arg_5_0, arg_5_1)
	if arg_5_0.focused_node then
		local var_5_0 = arg_5_0.focused_node:getChildByName("Label")

		if var_5_0 then
			local var_5_1 = var_5_0:getComponent(LabelComponent)

			if var_5_1 then
				var_5_1:setText(arg_5_1)
			end
		end
	end

	if arg_5_0.unfocused_node then
		local var_5_2 = arg_5_0.unfocused_node:getChildByName("Label")

		if var_5_2 then
			local var_5_3 = var_5_2:getComponent(LabelComponent)

			if var_5_3 then
				var_5_3:setText(arg_5_1)
			end
		end
	end
end
