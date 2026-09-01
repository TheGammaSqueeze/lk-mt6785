require("../core/core.lua")
require("../core/pool.lua")

TweenLoopNode = class()

createPooledNewAndFreeMethods(TweenLoopNode, true)

function TweenLoopNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.curLoops = 0
	arg_1_0.loopCount = arg_1_1.loopCount
	arg_1_0.tween = arg_1_1.tween.klass.new(arg_1_1.tween, arg_1_2)
end

function TweenLoopNode.destroy(arg_2_0)
	arg_2_0.tween:free()
end

function TweenLoopNode.getDuration(arg_3_0)
	return arg_3_0.loopCount * arg_3_0.tween:getDuration()
end

function TweenLoopNode.reset(arg_4_0)
	arg_4_0.curLoops = 0

	arg_4_0.tween:reset()
end

function TweenLoopNode.update(arg_5_0, arg_5_1)
	while arg_5_1 > 0 and arg_5_0.curLoops < arg_5_0.loopCount do
		local var_5_0
		local var_5_1

		arg_5_1, var_5_1 = arg_5_0.tween:update(arg_5_1)

		if var_5_1 then
			arg_5_0.curLoops = arg_5_0.curLoops + 1

			if arg_5_0.curLoops < arg_5_0.loopCount then
				arg_5_0.tween:reset()
			end
		end
	end

	return arg_5_1, arg_5_0.curLoops >= arg_5_0.loopCount
end
