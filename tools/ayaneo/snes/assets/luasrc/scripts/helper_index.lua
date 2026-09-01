local function var_0_0(arg_1_0, arg_1_1, arg_1_2)
	local var_1_0 = arg_1_2 or 1
	local var_1_1 = arg_1_0 + var_1_0

	if var_1_0 < 0 then
		var_1_0 = var_1_0 % arg_1_1 + arg_1_1
	end

	arg_1_0 = (arg_1_0 + var_1_0) % arg_1_1

	local var_1_2 = arg_1_0 ~= var_1_1

	return arg_1_0, var_1_2
end

function index_loop_next(arg_2_0, arg_2_1, arg_2_2)
	local var_2_0 = arg_2_0 - 1
	local var_2_1 = arg_2_1
	local var_2_2 = arg_2_2 or 1
	local var_2_3 = var_2_0 + var_2_2

	if var_2_2 < 0 then
		var_2_2 = var_2_2 % var_2_1 + var_2_1
	end

	local var_2_4 = (var_2_0 + var_2_2) % var_2_1
	local var_2_5 = var_2_4 ~= var_2_3

	return var_2_4 + 1, var_2_5
end

function index_loop_prev(arg_3_0, arg_3_1, arg_3_2)
	local var_3_0 = -(arg_3_2 or 1)

	return index_loop_next(arg_3_0, arg_3_1, var_3_0)
end

function index_next(arg_4_0, arg_4_1, arg_4_2)
	local var_4_0 = arg_4_0 - 1
	local var_4_1 = arg_4_1
	local var_4_2 = arg_4_2 or 1

	if var_4_2 < 0 then
		var_4_2 = var_4_2 % var_4_1 + var_4_1
	end

	local var_4_3 = (var_4_0 + var_4_2) % var_4_1

	if var_4_3 < 0 then
		return 1, true
	elseif var_4_1 < var_4_3 then
		return var_4_1, true
	else
		return var_4_3 + 1, false
	end
end

function index_prev(arg_5_0, arg_5_1, arg_5_2)
	local var_5_0 = -(arg_5_2 or 1)

	return index_next(arg_5_0, arg_5_1, var_5_0)
end

function index_loop_next_ex(arg_6_0, arg_6_1, arg_6_2, arg_6_3)
	local var_6_0 = arg_6_0 - arg_6_1
	local var_6_1 = arg_6_2 - arg_6_1 + 1
	local var_6_2, var_6_3 = var_0_0(var_6_0, var_6_1, arg_6_3)

	return var_6_2 + arg_6_1, var_6_3
end

function index_loop_prev_ex(arg_7_0, arg_7_1, arg_7_2, arg_7_3)
	local var_7_0 = -(arg_7_3 or 1)

	return index_loop_next_ex(arg_7_0, arg_7_1, arg_7_2, var_7_0)
end

function index_vertical_grid(arg_8_0, arg_8_1, arg_8_2, arg_8_3, arg_8_4)
	local var_8_0 = arg_8_0 - 1
	local var_8_1 = arg_8_1
	local var_8_2
	local var_8_3 = math.floor(var_8_0 / arg_8_2)
	local var_8_4 = var_8_0 % arg_8_2
	local var_8_5 = math.floor(var_8_1 / arg_8_2)

	if var_8_4 <= var_8_1 % arg_8_2 then
		var_8_5 = var_8_5 + 1
	end

	if arg_8_3 == "row" then
		var_8_3, var_8_2 = var_0_0(var_8_3, var_8_5, arg_8_4)
	elseif arg_8_3 == "column" then
		var_8_4, var_8_2 = var_0_0(var_8_4, arg_8_2, arg_8_4)
	end

	return var_8_3 * arg_8_2 + var_8_4 + 1, var_8_2
end
