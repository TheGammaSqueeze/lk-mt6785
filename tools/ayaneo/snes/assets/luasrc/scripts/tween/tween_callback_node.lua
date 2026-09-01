require("../core/core.lua")
require("../core/pool.lua")

TweenCallbackNode = class()

createPooledNewAndFreeMethods(TweenCallbackNode, true)

function TweenCallbackNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.func = arg_1_1.func
	arg_1_0.argCount = arg_1_1.argCount
	arg_1_0.args = arg_1_1.args
end

function TweenCallbackNode.getDuration(arg_2_0)
	return 0
end

function TweenCallbackNode.reset(arg_3_0)
	return
end

function TweenCallbackNode.update(arg_4_0, arg_4_1)
	local var_4_0, var_4_1 = xpcall(function()
		arg_4_0.func(unpack(arg_4_0.args, 1, arg_4_0.argCount))
	end, debug.traceback)

	if not var_4_0 then
		print(var_4_1)
	end

	return arg_4_1, true
end
