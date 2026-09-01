function class(arg_1_0)
	local var_1_0 = {}

	var_1_0.__index = var_1_0

	setmetatable(var_1_0, arg_1_0)

	return var_1_0
end

function new(arg_2_0)
	local var_2_0 = {}

	setmetatable(var_2_0, arg_2_0)

	return var_2_0
end

function set(arg_3_0)
	local var_3_0 = {}

	for iter_3_0 = 1, #arg_3_0 do
		var_3_0[arg_3_0[iter_3_0]] = true
	end

	return var_3_0
end

function map(arg_4_0, arg_4_1)
	local var_4_0 = {}

	for iter_4_0, iter_4_1 in ipairs(arg_4_0) do
		var_4_0[iter_4_0] = arg_4_1(iter_4_1)
	end

	return var_4_0
end

function breakOnObject(arg_5_0)
	local var_5_0 = getmetatable(arg_5_0)

	while var_5_0 do
		for iter_5_0, iter_5_1 in pairs(var_5_0) do
			if type(iter_5_1) == "function" then
				var_5_0[iter_5_0] = function(...)
					if select(1, ...) == arg_5_0 then
						print("put a breakpoint here")
					end

					return iter_5_1(...)
				end
			end
		end

		var_5_0 = getmetatable(var_5_0)
	end
end
