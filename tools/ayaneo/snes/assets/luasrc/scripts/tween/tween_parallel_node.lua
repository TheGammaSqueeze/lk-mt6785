require("../core/core.lua")
require("../core/pool.lua")

TweenParallelNode = class()

createPooledNewAndFreeMethods(TweenParallelNode, true)

local var_0_0 = new(Pool)

function TweenParallelNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.elems = var_0_0:alloc()

	for iter_1_0 = 1, #arg_1_1.elems do
		local var_1_0 = arg_1_1.elems[iter_1_0].klass.new(arg_1_1.elems[iter_1_0], arg_1_2)

		arg_1_0.elems[#arg_1_0.elems + 1] = var_1_0
		var_1_0.finished = false
	end
end

function TweenParallelNode.destroy(arg_2_0)
	for iter_2_0 = 1, #arg_2_0.elems do
		arg_2_0.elems[iter_2_0]:free()
	end

	var_0_0:free(arg_2_0.elems)
end

function TweenParallelNode.getDuration(arg_3_0)
	local var_3_0 = 0

	for iter_3_0 = 1, #arg_3_0.elems do
		var_3_0 = math.max(var_3_0, arg_3_0.elems[iter_3_0]:getDuration())
	end

	return var_3_0
end

function TweenParallelNode.reset(arg_4_0)
	for iter_4_0 = 1, #arg_4_0.elems do
		arg_4_0.elems[iter_4_0]:reset()

		arg_4_0.elems[iter_4_0].finished = false
	end
end

function TweenParallelNode.update(arg_5_0, arg_5_1)
	local var_5_0 = arg_5_1
	local var_5_1 = true

	for iter_5_0 = 1, #arg_5_0.elems do
		if not arg_5_0.elems[iter_5_0].finished then
			local var_5_2, var_5_3 = arg_5_0.elems[iter_5_0]:update(arg_5_1)

			var_5_0 = math.min(var_5_0, var_5_2)
			arg_5_0.elems[iter_5_0].finished = var_5_3

			if not var_5_3 then
				var_5_1 = false
			end
		end
	end

	return var_5_0, var_5_1
end
