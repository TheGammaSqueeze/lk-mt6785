require("/scripts/core/core.lua")

sys_game_card_show = class(WorldNode)

function sys_game_card_show.start(arg_1_0)
	arg_1_0.enabled_icon = {
		arg_1_0.resume_icon1e,
		arg_1_0.resume_icon2e,
		arg_1_0.resume_icon3e,
		arg_1_0.resume_icon4e
	}
	arg_1_0.disabled_icon = {
		arg_1_0.resume_icon1d,
		arg_1_0.resume_icon2d,
		arg_1_0.resume_icon3d,
		arg_1_0.resume_icon4d
	}
	arg_1_0.locked_icon = {
		arg_1_0.resume_icon1l,
		arg_1_0.resume_icon2l,
		arg_1_0.resume_icon3l,
		arg_1_0.resume_icon4l
	}
end

function sys_game_card_show.setPlayers(arg_2_0, arg_2_1, arg_2_2)
	local var_2_0 = table_wrapNullable

	if arg_2_1 == 1 then
		var_2_0(arg_2_0).player1_icon:enable()
		var_2_0(arg_2_0).player2_icon:disable()
		var_2_0(arg_2_0).player2s_icon:disable()
	elseif arg_2_2 then
		var_2_0(arg_2_0).player1_icon:disable()
		var_2_0(arg_2_0).player2_icon:disable()
		var_2_0(arg_2_0).player2s_icon:enable()
	else
		var_2_0(arg_2_0).player1_icon:disable()
		var_2_0(arg_2_0).player2_icon:enable()
		var_2_0(arg_2_0).player2s_icon:disable()
	end
end

function sys_game_card_show.setResumeInfo(arg_3_0, arg_3_1)
	local var_3_0 = table_wrapNullable

	for iter_3_0 = 1, 4 do
		if SuspentionPoints:getInfo(arg_3_1, iter_3_0) then
			var_3_0(arg_3_0).disabled_icon[iter_3_0]:disable()

			if system.is_locked_resumedata(arg_3_1, iter_3_0) then
				var_3_0(arg_3_0).enabled_icon[iter_3_0]:disable()
				var_3_0(arg_3_0).locked_icon[iter_3_0]:enable()
			else
				var_3_0(arg_3_0).enabled_icon[iter_3_0]:enable()
				var_3_0(arg_3_0).locked_icon[iter_3_0]:disable()
			end
		else
			var_3_0(arg_3_0).enabled_icon[iter_3_0]:disable()
			var_3_0(arg_3_0).locked_icon[iter_3_0]:disable()
			var_3_0(arg_3_0).disabled_icon[iter_3_0]:enable()
		end
	end
end
