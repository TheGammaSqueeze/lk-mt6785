require("../core/core.lua")
require("../core/pool.lua")

TweenSequenceNode = class()

createPooledNewAndFreeMethods(TweenSequenceNode, true)

local var_0_0 = new(Pool)

function TweenSequenceNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.currentElem = 1
	arg_1_0.elems = var_0_0:alloc()
	arg_1_0.speed = 1

	for iter_1_0 = 1, #arg_1_1.elems do
		local var_1_0 = arg_1_1.elems[iter_1_0].klass.new(arg_1_1.elems[iter_1_0], arg_1_2)

		arg_1_0.elems[iter_1_0] = var_1_0
	end
end

function TweenSequenceNode.destroy(arg_2_0)
	for iter_2_0 = 1, #arg_2_0.elems do
		arg_2_0.elems[iter_2_0]:free()
	end

	var_0_0:free(arg_2_0.elems)
end

function TweenSequenceNode.getDuration(arg_3_0)
	local var_3_0 = 0

	for iter_3_0 = 1, #arg_3_0.elems do
		var_3_0 = var_3_0 + arg_3_0.elems[iter_3_0]:getDuration()
	end

	return var_3_0
end

function TweenSequenceNode.reset(arg_4_0)
	arg_4_0.currentElem = 1

	if #arg_4_0.elems > 0 then
		arg_4_0.elems[1]:reset()
	end
end

function TweenSequenceNode.update(arg_5_0, arg_5_1)
	local var_5_0 = false

	arg_5_1 = arg_5_1 * arg_5_0.speed

	while arg_5_1 > 0 and arg_5_0.currentElem <= #arg_5_0.elems do
		local var_5_1

		arg_5_1, var_5_1 = arg_5_0.elems[arg_5_0.currentElem]:update(arg_5_1)

		if var_5_1 then
			arg_5_0.currentElem = arg_5_0.currentElem + 1

			if arg_5_0.currentElem <= #arg_5_0.elems then
				arg_5_0.elems[arg_5_0.currentElem]:reset()
			end
		end
	end

	return arg_5_1 / arg_5_0.speed, arg_5_0.currentElem > #arg_5_0.elems
end
