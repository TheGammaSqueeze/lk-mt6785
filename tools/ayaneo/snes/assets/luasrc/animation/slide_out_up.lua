require("/scripts/core/core.lua")
require("/animation/tweenanimator.lua")

slide_out_up = class(tweenAnimator)

function slide_out_up.initialize(arg_1_0)
	local var_1_0 = ScriptComponent.getNode(arg_1_0)

	if var_1_0 then
		var_1_0:setWorldPosition(0, 720)
	end
end

function slide_out_up.getTween(arg_2_0, arg_2_1)
	local var_2_0 = arg_2_0.delay or 0

	return (Tween_noControl(Tween:wait(var_2_0), Tween:animate(arg_2_1, 0.2, "to", {
		WorldY = 720,
		WorldX = 0
	}, Ease.inExpo), Tween:worldNodeEnabledTo(arg_2_1, 0, false)))
end
