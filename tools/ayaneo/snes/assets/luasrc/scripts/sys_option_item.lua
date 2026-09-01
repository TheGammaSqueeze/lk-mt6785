require("/scripts/core/core.lua")

sys_option_item = class(GUICheckButton)

function sys_option_item.start(arg_1_0)
	GUICheckButton.start(arg_1_0)

	if arg_1_0.focused_node then
		arg_1_0.focused_node:disable()
	end

	if arg_1_0.squareCursor == nil then
		arg_1_0.squareCursor = false
	end
end

function sys_option_item.onCommand(arg_2_0, arg_2_1)
	GUICheckButton.onCommand(arg_2_0, arg_2_1)

	if arg_2_1.id == GUI_COMMAND_FOCUS then
		arg_2_0:activate()
	elseif arg_2_1.id == GUI_COMMAND_UNFOCUS then
		arg_2_0:deactivate()
	elseif arg_2_1.id == GUI_COMMAND_CLICK and arg_2_0.listener ~= nil and arg_2_0.listener.onButtonClick ~= nil then
		arg_2_0.listener:onButtonClick(arg_2_0)
	end
end

function sys_option_item.activate(arg_3_0)
	if arg_3_0.focused_node then
		arg_3_0.focused_node:enable()
	end

	if system.cursor and arg_3_0.cursor_component then
		if arg_3_0.squareCursor then
			system.cursor:setSquare(arg_3_0.cursor_component)
		else
			system.cursor:setRounded(arg_3_0.cursor_component)
		end
	end
end

function sys_option_item.deactivate(arg_4_0)
	if arg_4_0.focused_node then
		arg_4_0.focused_node:disable()
	end
end
