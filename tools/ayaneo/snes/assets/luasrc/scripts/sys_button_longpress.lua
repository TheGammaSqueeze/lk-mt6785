require("/scripts/core/core.lua")
require("/scripts/app/clover_pad.lua")

sys_button_longpress = class(sys_button)

function sys_button_longpress.start(arg_1_0)
	sys_button.start(arg_1_0)

	arg_1_0.gauge_component = arg_1_0.gauge:getComponent(VisualComponent)

	arg_1_0.gauge:setLocalScale(0, 0)
	arg_1_0.gauge:disable()

	arg_1_0.gauge_l_component = arg_1_0.gauge_l:getComponent(VisualComponent)

	arg_1_0.gauge_l:disable()

	arg_1_0.gauge_r_component = arg_1_0.gauge_r:getComponent(VisualComponent)

	arg_1_0.gauge_r:disable()
end

function sys_button_longpress.update(arg_2_0, arg_2_1)
	GUIButton.update(arg_2_0, arg_2_1)

	if arg_2_0 == GUI.focusedElement and not GUI.disabled then
		if CloverPadUI:isValidatePressed() then
			-- block empty
		elseif arg_2_0.longpress_delay ~= nil and CloverPadUI:isValidateDown() then
			arg_2_0.longpress_delay = arg_2_0.longpress_delay + arg_2_1

			if arg_2_0.longpress_delay >= arg_2_0.longpress_sec then
				arg_2_0.longpress_delay = nil
				arg_2_0.longpress_isFull = true

				arg_2_0:setGaugeRate(1)
				arg_2_0:click()
			else
				local var_2_0 = arg_2_0.longpress_delay / arg_2_0.longpress_sec

				arg_2_0:setGaugeRate(var_2_0)
			end
		elseif CloverPadUI:isValidateReleased() then
			arg_2_0:setGaugeRate(0)

			arg_2_0.longpress_delay = nil
		end
	elseif arg_2_0.longpress_delay then
		arg_2_0.longpress_delay = nil

		arg_2_0:setGaugeRate(0)
	end
end

function sys_button_longpress.activate(arg_3_0)
	sys_button.activate(arg_3_0)
	arg_3_0:setGaugeRate(0)

	arg_3_0.longpress_isFull = false

	if arg_3_0.longpress_ignore then
		arg_3_0.longpress_ignore:enable()
	end
end

function sys_button_longpress.deactivate(arg_4_0)
	if not arg_4_0.longpress_isFull then
		sys_button.deactivate(arg_4_0)
	elseif arg_4_0.longpress_ignore then
		arg_4_0.longpress_ignore:disable()
	end
end

function sys_button_longpress.onCommand(arg_5_0, arg_5_1)
	if arg_5_1.id == GUI_COMMAND_CLICK then
		arg_5_0.longpress_delay = 0

		return GUIElement.onCommand(arg_5_0, arg_5_1)
	else
		return GUIButton.onCommand(arg_5_0, arg_5_1)
	end
end

function sys_button_longpress.setGaugeRate(arg_6_0, arg_6_1)
	if arg_6_1 <= 0 then
		arg_6_0.gauge:disable()
		arg_6_0.gauge_l:disable()
		arg_6_0.gauge_r:disable()
	else
		arg_6_0.gauge:enable()
		arg_6_0.gauge_l:enable()
		arg_6_0.gauge_r:enable()
		arg_6_0.gauge:setLocalScale(arg_6_1, 1)

		local var_6_0, var_6_1 = arg_6_0.gauge_component:getSize()
		local var_6_2 = var_6_0 * arg_6_1
		local var_6_3, var_6_4 = arg_6_0.gauge_l_component:getSize()
		local var_6_5, var_6_6 = arg_6_0.gauge_r_component:getSize()

		arg_6_0.gauge_l:setLocalPosition(-(var_6_2 / 2 + var_6_3 / 2), 0)
		arg_6_0.gauge_r:setLocalPosition(var_6_2 / 2 + var_6_5 / 2, 0)
	end
end
