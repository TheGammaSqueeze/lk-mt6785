require("/scripts/core/core.lua")

tweenAnimator = class()

function tweenAnimator.getTween(arg_1_0, arg_1_1)
	return Tween:sequence()
end

function tweenAnimator.update(arg_2_0, arg_2_1)
	if not arg_2_0.is_initialized and arg_2_0.initialize then
		arg_2_0:initialize()

		arg_2_0.is_initialized = true
	end

	if arg_2_0.autoplay and not arg_2_0.tweenInst then
		local var_2_0 = ScriptComponent.getNode(arg_2_0)

		arg_2_0.tweenInst = arg_2_0:run(var_2_0)
	end
end

function tweenAnimator.run(arg_3_0, arg_3_1)
	if not arg_3_0.is_initialized and arg_3_0.initialize then
		arg_3_0:initialize()

		arg_3_0.is_initialized = true
	end

	arg_3_0:stopAnim()

	local var_3_0 = arg_3_0:getTween(arg_3_1)
	local var_3_1

	if arg_3_0.looped then
		var_3_1 = Tween:loop(math.huge, var_3_0):start()
	else
		var_3_1 = var_3_0:start()
	end

	arg_3_0.tweenInst = var_3_1

	return var_3_1
end

function tweenAnimator.isRunning(arg_4_0)
	if arg_4_0.tweenInst and TweenManager:isTweenActive(arg_4_0.tweenInst) then
		return true
	end

	return false
end

function tweenAnimator.stopAnim(arg_5_0)
	tween_stop(arg_5_0.tweenInst)
end
