require("/scripts/core/core.lua")
require("/animation/tweenanimator.lua")

blink_anim = class(tweenAnimator)

function blink_anim.getTween(arg_1_0, arg_1_1)
	return Tween:sequence(Tween:wait(0.8), Tween:worldNodeEnabledTo(arg_1_1, 0, true), Tween:wait(1), Tween:worldNodeEnabledTo(arg_1_1, 0, false))
end
