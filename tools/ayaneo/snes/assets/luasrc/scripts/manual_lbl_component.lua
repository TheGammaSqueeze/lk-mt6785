require("/scripts/core/core.lua")

manual_lbl_component = class(LabelComponent)

function manual_lbl_component.start(arg_1_0)
	if arg_1_0.tags then
		for iter_1_0, iter_1_1 in pairs(arg_1_0.tags) do
			arg_1_0:setLocalVariable(iter_1_0, iter_1_1)

			local var_1_0 = 0
			local var_1_1 = 0

			if iter_1_0 == "Btn_X" then
				local var_1_2 = -10, -6
			elseif iter_1_0 == "Icon_Display" then
				local var_1_3 = -25, -3
			elseif iter_1_0 == "Icon_Settings" then
				local var_1_4 = -30, 0
			elseif iter_1_0 == "Icon_Copyright" then
				local var_1_5 = -30, 0
			elseif iter_1_0 == "Icon_Manual" then
				local var_1_6 = -30, 0
			elseif iter_1_0 == "Icon_lock" then
				local var_1_7 = -13, -8
			elseif iter_1_0 == "Btn_A" then
				local var_1_8 = -10, -6
			elseif iter_1_0 == "Btn_CrossBottom" then
				local var_1_9 = -10, -6
			elseif iter_1_0 == "Btn_SELECT" then
				local var_1_10 = -30, -10
			elseif iter_1_0 == "Icon_TrashCan" then
				local var_1_11 = -30, -10
			elseif iter_1_0 == "Btn_L" then
				local var_1_12 = -12, -6
			elseif iter_1_0 == "Btn_R" then
				local var_1_13 = -12, -6
			elseif iter_1_0 == "Btn_START" then
				local var_1_14 = -30, 0
			elseif iter_1_0 == "Btn_B" then
				local var_1_15 = -30, -3
			end

			local var_1_16, var_1_17 = 0, 0

			arg_1_0:setTextOffset(var_1_16, var_1_17)
		end
	end
end
