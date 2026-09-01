require("/scripts/core/core.lua")
require("/scripts/suspentionpoints.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_autocancel.lua")

sys_resume_emptymode = class(gui_container)

function sys_resume_emptymode.activate(arg_1_0)
	gui_container.activate(arg_1_0)

	if system.cursor then
		if arg_1_0.cursor_component then
			system.cursor:setSquare(arg_1_0.cursor_component)
		else
			system.cursor:setRounded(system.game_card.cursor_component, true)
		end
	end

	if arg_1_0.explanation then
		arg_1_0:showExplanation()
	end

	CloverAutoCancel.Set("sys_resume_emptymode", arg_1_0)
end

function sys_resume_emptymode.onButtonClick(arg_2_0, arg_2_1)
	if table_find(arg_2_0.elementArray, arg_2_1) and arg_2_0.failureSound then
		arg_2_0.failureSound:stop()
		arg_2_0.failureSound:play()
	end
end

function sys_resume_emptymode.onElementCommand(arg_3_0, arg_3_1, arg_3_2)
	local var_3_0 = arg_3_0.elementArray

	if table_find(var_3_0, arg_3_1) and arg_3_2.id == GUI_COMMAND_KEYPRESS and (arg_3_0.upElement == nil or not arg_3_0.upElement:isEnabled()) and CloverPadUI.pressed.up then
		arg_3_0:Back()

		return true
	end

	return gui_container.onElementCommand(arg_3_0, arg_3_1, arg_3_2)
end

function sys_resume_emptymode.onContainerCanceled(arg_4_0, arg_4_1)
	arg_4_0:Back()
end

function sys_resume_emptymode.Back(arg_5_0, arg_5_1)
	CloverAutoCancel.Set("sys_resume_emptymode", nil)
	Main:toHomeMenu()

	if arg_5_0.cancelSound then
		arg_5_0.cancelSound:stop()
		arg_5_0.cancelSound:play()
	end
end

function sys_resume_emptymode.showExplanation(arg_6_0)
	arg_6_0.explanation:enable()

	local var_6_0 = 1
	local var_6_1 = {}

	local function var_6_2(arg_7_0)
		for iter_7_0 in iterate_children(arg_7_0) do
			local var_7_0 = iter_7_0:getComponents(VisualComponent)

			for iter_7_1, iter_7_2 in ipairs(var_7_0) do
				iter_7_2:setAlpha(1)

				local var_7_1 = Tween:alphaTo(iter_7_2, var_6_0, 0)

				table.insert(var_6_1, var_7_1)
			end

			var_6_2(iter_7_0)
		end
	end

	var_6_2(arg_6_0.explanation)
	tween_stop(arg_6_0.tween)

	arg_6_0.tween = Tween:sequence(Tween:wait(2), Tween:parallel(unpack(var_6_1)), Tween:worldNodeEnabledTo(arg_6_0.explanation, 0, false)):start()
end
