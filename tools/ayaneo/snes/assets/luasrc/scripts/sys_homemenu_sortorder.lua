require("/scripts/core/core.lua")

sys_homemenu_sortorder = class(WorldNode)

function sys_homemenu_sortorder.showSortOrder(arg_1_0, arg_1_1)
	arg_1_0:enable()

	local var_1_0 = 1
	local var_1_1 = {}

	local function var_1_2(arg_2_0)
		for iter_2_0 in iterate_children(arg_2_0) do
			local var_2_0 = iter_2_0:getComponents(VisualComponent)

			for iter_2_1, iter_2_2 in ipairs(var_2_0) do
				iter_2_2:setAlpha(1)

				local var_2_1 = Tween:alphaTo(iter_2_2, var_1_0, 0)

				table.insert(var_1_1, var_2_1)
			end

			var_1_2(iter_2_0)
		end
	end

	var_1_2(arg_1_0)
	arg_1_0.label:setText(arg_1_1)
	tween_stop(arg_1_0.tween)

	arg_1_0.tween = Tween:sequence(Tween:wait(1), Tween:parallel(unpack(var_1_1)), Tween:worldNodeEnabledTo(arg_1_0, 0, false)):start()
end

function sys_homemenu_sortorder.showReplayText(arg_3_0)
	arg_3_0:enable()

	local var_3_0 = arg_3_0:getZIndex()

	arg_3_0:setZIndex(210)

	local function var_3_1(arg_4_0)
		for iter_4_0 in iterate_children(arg_4_0) do
			local var_4_0 = iter_4_0:getComponents(VisualComponent)

			for iter_4_1, iter_4_2 in ipairs(var_4_0) do
				iter_4_2:setAlpha(1)
			end

			var_3_1(iter_4_0)
		end
	end

	var_3_1(arg_3_0)
	tween_stop(arg_3_0.tween)
	arg_3_0.label:setText(Localization.getText("sys_resume_Replay"))

	arg_3_0.tween = Tween:sequence(Tween:worldNodeEnabledTo(arg_3_0, 1, false), Tween:callback(function()
		arg_3_0:setZIndex(var_3_0)
	end)):start()
end
