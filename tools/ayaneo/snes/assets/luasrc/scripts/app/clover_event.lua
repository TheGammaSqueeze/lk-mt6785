require("/scripts/core/core.lua")

CloverEvent = class()

function CloverEvent.new()
	local var_1_0 = new(CloverEvent)

	var_1_0:reset()

	return var_1_0
end

function CloverEvent.reset(arg_2_0)
	arg_2_0.index = 1
	arg_2_0.dt = 0
	arg_2_0.wait_time = 0
end

function CloverEvent.update(arg_3_0, arg_3_1, arg_3_2)
	if arg_3_0.index > #arg_3_1 then
		return
	end

	arg_3_0.dt = arg_3_2

	if arg_3_0.wait_time == 0 then
		arg_3_1[arg_3_0.index]()
	else
		arg_3_0.wait_time = arg_3_0.wait_time - arg_3_2

		if arg_3_0.wait_time < 0 then
			arg_3_0.wait_time = 0
		end
	end
end

function CloverEvent.wait(arg_4_0, arg_4_1)
	arg_4_0.wait_time = arg_4_1
end

function CloverEvent.next_index(arg_5_0)
	arg_5_0.index = arg_5_0.index + 1
end
