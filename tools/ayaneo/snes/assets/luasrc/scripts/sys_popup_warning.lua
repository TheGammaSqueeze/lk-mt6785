require("/scripts/core/core.lua")
require("/scripts/app/clover_const.lua")

sys_popup_warning = class(WorldNode)

function sys_popup_warning.activate(arg_1_0, arg_1_1)
	if arg_1_0.warning_type then
		arg_1_0:setWarningType(arg_1_0.warning_type)
	end

	if arg_1_1 then
		arg_1_0.popup_tween = Tween:sequence(Tween:wait(arg_1_1), Tween:callback(function()
			arg_1_0:enable()
			arg_1_0:setLocalScale(0.5, 0.5)
		end), Tween:scaleTo(arg_1_0, CloverConst.Anim.POPUP_SCALING_SECONDS, 1, 1)):start()
	else
		arg_1_0:enable()
	end
end

function sys_popup_warning.deactivate(arg_3_0)
	tween_stop(arg_3_0.popup_tween)
	arg_3_0:setLocalScale(1, 1)
	arg_3_0:disable()
end

function sys_popup_warning.setWarningType(arg_4_0, arg_4_1)
	local var_4_0 = {
		copy = function(arg_5_0)
			arg_5_0.warning_label:setText(Localization.getText("sys_resume_attention"))
			arg_5_0.icon:disable()
			arg_5_0.sram:enable()
		end,
		register = function(arg_6_0)
			arg_6_0.warning_label:setText(Localization.getText("sys_resume_explanation_3"))
			arg_6_0.icon:enable()
			arg_6_0.sram:disable()
		end
	}

	assert(var_4_0[arg_4_1])
	var_4_0[arg_4_1](arg_4_0)
end
