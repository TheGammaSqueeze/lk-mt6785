function table_find(arg_1_0, arg_1_1)
	if type(arg_1_0) ~= "table" or arg_1_1 == nil then
		return nil
	end

	for iter_1_0, iter_1_1 in pairs(arg_1_0) do
		if rawequal(iter_1_1, arg_1_1) then
			return iter_1_0
		end
	end

	return nil
end

function table_find_if(arg_2_0, arg_2_1)
	if type(arg_2_0) ~= "table" or type(arg_2_1) ~= "function" then
		return nil
	end

	for iter_2_0, iter_2_1 in pairs(arg_2_0) do
		if arg_2_1(iter_2_1) then
			return iter_2_0
		end
	end

	return nil
end

function table_isEmpty(arg_3_0)
	if type(arg_3_0) ~= "table" then
		return true
	end

	return next(arg_3_0) == nil
end

function table_toStringKey(arg_4_0)
	local var_4_0 = {}

	for iter_4_0, iter_4_1 in pairs(arg_4_0) do
		if type(iter_4_0) == "number" then
			var_4_0[iter_4_0] = ("_%d"):format(iter_4_0)
		end

		if type(iter_4_1) == "table" then
			table_toStringKey(iter_4_1)
		end
	end

	for iter_4_2, iter_4_3 in pairs(var_4_0) do
		arg_4_0[iter_4_3] = arg_4_0[iter_4_2]
		arg_4_0[iter_4_2] = nil
	end
end

function table_toNumberKey(arg_5_0)
	local var_5_0 = {}

	for iter_5_0, iter_5_1 in pairs(arg_5_0) do
		if type(iter_5_0) == "string" then
			local var_5_1 = iter_5_0:match("^_(%d+)$")

			if var_5_1 then
				var_5_0[iter_5_0] = tonumber(var_5_1)
			end
		end

		if type(iter_5_1) == "table" then
			table_toNumberKey(iter_5_1)
		end
	end

	for iter_5_2, iter_5_3 in pairs(var_5_0) do
		arg_5_0[iter_5_3] = arg_5_0[iter_5_2]
		arg_5_0[iter_5_2] = nil
	end
end

local var_0_0 = {}

local function var_0_1(arg_6_0, arg_6_1)
	if arg_6_0 == var_0_0 then
		return arg_6_1
	else
		return arg_6_0
	end
end

local var_0_2 = {
	__add = function(arg_7_0, arg_7_1)
		return var_0_1(arg_7_0, 0) + var_0_1(arg_7_1, 0)
	end,
	__sub = function(arg_8_0, arg_8_1)
		return var_0_1(arg_8_0, 0) - var_0_1(arg_8_1, 0)
	end,
	__mul = function(arg_9_0, arg_9_1)
		return var_0_1(arg_9_0, 0) * var_0_1(arg_9_1, 1)
	end,
	__div = function(arg_10_0, arg_10_1)
		return var_0_1(arg_10_0, 0) / var_0_1(arg_10_1, 1)
	end,
	__mod = function(arg_11_0, arg_11_1)
		return var_0_1(arg_11_0, 0) % var_0_1(arg_11_1, 1)
	end,
	__pow = function(arg_12_0, arg_12_1)
		return var_0_1(arg_12_0, 0)^var_0_1(arg_12_1, 1)
	end,
	__unm = function(arg_13_0)
		return var_0_0
	end,
	__concat = function(arg_14_0, arg_14_1)
		return var_0_1(arg_14_0, "") .. var_0_1(arg_14_1, "")
	end,
	__len = function(arg_15_0)
		return 0
	end,
	__index = function(arg_16_0, arg_16_1, arg_16_2)
		return var_0_0
	end,
	__newindex = function(arg_17_0, arg_17_1, arg_17_2)
		return var_0_0
	end,
	__call = function(arg_18_0)
		return var_0_0
	end,
	__tostring = function(arg_19_0)
		return ""
	end
}

var_0_2.__metatable = var_0_2

setmetatable(var_0_0, var_0_2)

local function var_0_3(arg_20_0)
	local var_20_0 = getmetatable(arg_20_0)

	if var_20_0 and var_20_0.nullableReference then
		return true
	else
		return false
	end
end

local function var_0_4(arg_21_0, arg_21_1)
	local var_21_0 = table_unwrapNullable(arg_21_0)

	return table_wrapNullable(var_21_0[arg_21_1])
end

local function var_0_5(arg_22_0, arg_22_1, arg_22_2)
	table_unwrapNullable(arg_22_0)[arg_22_1] = arg_22_2
end

local function var_0_6(arg_23_0, arg_23_1, ...)
	local var_23_0 = table_unwrapNullable(arg_23_0)
	local var_23_1 = table_unwrapNullable(arg_23_1)

	return table_wrapNullable(var_23_0(var_23_1, ...))
end

function table_wrapNullable(arg_24_0)
	local var_24_0 = getmetatable(arg_24_0)

	if arg_24_0 == nil then
		return var_0_0
	elseif var_24_0 and var_24_0.nullableReference then
		return arg_24_0
	else
		local var_24_1 = {}
		local var_24_2 = {
			__index = var_0_4,
			__newindex = var_0_6,
			__call = var_0_6,
			nullableReference = arg_24_0
		}

		setmetatable(var_24_1, var_24_2)

		return var_24_1
	end
end

function table_unwrapNullable(arg_25_0)
	local var_25_0 = getmetatable(arg_25_0)

	if var_25_0 and var_25_0.nullableReference then
		return var_25_0.nullableReference
	elseif arg_25_0 == var_0_0 then
		return nil
	else
		return arg_25_0
	end
end
