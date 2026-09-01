require("/scripts/core/core.lua")

CloverTask = {
	list = {},
	Init = function()
		return
	end,
	Update = function(arg_2_0)
		for iter_2_0, iter_2_1 in pairs(CloverTask.list) do
			if iter_2_1 and iter_2_1.func and iter_2_1.sleep == false then
				if iter_2_1.klass then
					pcall(iter_2_1.func, iter_2_1.klass, arg_2_0)
				else
					pcall(iter_2_1.func, arg_2_0)
				end
			end
		end
	end,
	Register = function(arg_3_0, arg_3_1, arg_3_2)
		assert(CloverTask.list[arg_3_0] == nil)

		CloverTask.list[arg_3_0] = {
			sleep = false,
			func = arg_3_1,
			klass = arg_3_2
		}
	end,
	Unregister = function(arg_4_0)
		CloverTask.list[arg_4_0] = nil
	end,
	Sleep = function(arg_5_0)
		assert(CloverTask.list[arg_5_0] ~= nil)

		CloverTask.list[arg_5_0].sleep = true
	end,
	Awake = function(arg_6_0)
		assert(CloverTask.list[arg_6_0] ~= nil)

		CloverTask.list[arg_6_0].sleep = false
	end,
	IsSleep = function(arg_7_0)
		return CloverTask.list[arg_7_0].sleep
	end
}
