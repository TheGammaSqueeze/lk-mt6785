require("../core/core.lua")
require("../core/pool.lua")

TweenWaitConditionNode = class()

createPooledNewAndFreeMethods(TweenWaitConditionNode, true)

function TweenWaitConditionNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.conditionFunc = arg_1_1.func
	arg_1_0.argCount = arg_1_1.argCount
	arg_1_0.args = arg_1_1.args
end

function TweenWaitConditionNode.getDuration(arg_2_0)
	return 0
end

function TweenWaitConditionNode.reset(arg_3_0)
	return
end

function TweenWaitConditionNode.update(arg_4_0, arg_4_1)
	local var_4_0, var_4_1 = xpcall(function()
		return arg_4_0.conditionFunc(unpack(arg_4_0.args, 1, arg_4_0.argCount))
	end, debug.traceback)

	if var_4_0 then
		if var_4_1 then
			return arg_4_1, true
		else
			return 0, false
		end
	else
		print(var_4_1)

		return arg_4_1, true
	end
end
