require("/scripts/core/core.lua")
require("/animation/tweenanimator.lua")

LR_arrow = class(tweenAnimator)

function LR_arrow.initialize(arg_1_0)
	local var_1_0 = ScriptComponent.getNode(arg_1_0)

	if var_1_0 then
		arg_1_0.orgX, arg_1_0.orgY = var_1_0:getLocalPosition()
	end
end

function LR_arrow.getTween(arg_2_0, arg_2_1)
	local var_2_0 = 3

	if arg_2_0.isRight then
		var_2_0 = -var_2_0
	end

	local var_2_1 = arg_2_0.orgX or 0
	local var_2_2 = arg_2_0.orgY or 0

	return Tween:sequence(Tween:moveTo(arg_2_1, 0.5, var_2_1 - var_2_0, var_2_2, Ease.inOutSine), Tween:moveTo(arg_2_1, 0.5, var_2_1 + var_2_0, var_2_2, Ease.inOutSine))
end
