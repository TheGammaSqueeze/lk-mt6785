require("/scripts/core/core.lua")

sys_option_setting_item = class(GUIButton)

function sys_option_setting_item.start(arg_1_0)
	GUIButton.start(arg_1_0)
	arg_1_0:setCurrent(arg_1_0.current)
end

function sys_option_setting_item.setCurrent(arg_2_0, arg_2_1)
	local var_2_0 = table_wrapNullable

	if arg_2_1 then
		var_2_0(arg_2_0).onVisual:enable()
		var_2_0(arg_2_0).offVisual:disable()
	else
		var_2_0(arg_2_0).onVisual:disable()
		var_2_0(arg_2_0).offVisual:enable()
	end

	arg_2_0.current = arg_2_1
end

function sys_option_setting_item.setOn(arg_3_0)
	arg_3_0:setCurrent(true)
end

function sys_option_setting_item.setOff(arg_4_0)
	arg_4_0:setCurrent(false)
end

function sys_option_setting_item.toggle(arg_5_0)
	arg_5_0:setCurrent(not arg_5_0.current)
end

function sys_option_setting_item.activate(arg_6_0)
	table_wrapNullable(arg_6_0).focused_node:enable()
end

function sys_option_setting_item.deactivate(arg_7_0)
	table_wrapNullable(arg_7_0).focused_node:disable()
end
