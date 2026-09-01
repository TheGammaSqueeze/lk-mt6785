CloverLayout = {
	SELECTABLE_TITLE_POS_X = {},
	InitTitlePos = function(arg_1_0, arg_1_1)
		assert(#CloverLayout.SELECTABLE_TITLE_POS_X == 0)

		local var_1_0 = arg_1_1 - 1
		local var_1_1 = -arg_1_0 * (var_1_0 / 2)

		for iter_1_0 = 1, arg_1_1 do
			CloverLayout.SELECTABLE_TITLE_POS_X[iter_1_0] = var_1_1
			var_1_1 = var_1_1 + arg_1_0
		end
	end,
	GetFirstSelectableTitleX = function()
		return CloverLayout.SELECTABLE_TITLE_POS_X[1]
	end,
	GetLastSelectableTitleX = function()
		return CloverLayout.SELECTABLE_TITLE_POS_X[#CloverLayout.SELECTABLE_TITLE_POS_X]
	end,
	GetSelectableTitleIndex = function(arg_4_0)
		local var_4_0 = 1
		local var_4_1 = math.huge

		for iter_4_0, iter_4_1 in ipairs(CloverLayout.SELECTABLE_TITLE_POS_X) do
			local var_4_2 = math.abs(iter_4_1 - arg_4_0)

			if var_4_2 < var_4_1 then
				var_4_0 = iter_4_0
				var_4_1 = var_4_2
			end
		end

		return var_4_0
	end,
	GetSelectableTitleX = function(arg_5_0)
		return CloverLayout.SELECTABLE_TITLE_POS_X[arg_5_0]
	end
}
