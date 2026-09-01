require("/scripts/core/core.lua")
require("/animation/tweenanimator.lua")

slide_in = class(tweenAnimator)

function slide_in.getTween(arg_1_0, arg_1_1)
	local var_1_0 = arg_1_0.delay or 0

	return (Tween_noControl(Tween:wait(var_1_0), Tween:worldNodeEnabledTo(arg_1_1, 0, true), Tween:animate(arg_1_1, 0.2, "to", {
		WorldY = 0,
		WorldX = 0
	}, Ease.outExpo)))
end
