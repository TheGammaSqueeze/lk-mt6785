require("/scripts/core/core.lua")
require("/scripts/SuspentionPoints.lua")

local var_0_0 = {
	INDEX_NOT_FOUND = 0
}

MyplayData = {
	Init = function()
		if not store.myplay_order then
			store.myplay_order = {}
		end

		for iter_1_0 = #store.myplay_order, 1, -1 do
			local var_1_0 = store.myplay_order[iter_1_0]

			if SuspentionPoints:hasSuspentionPoint(var_1_0.game_code) == false or SuspentionPoints:getInfo(var_1_0.game_code, var_1_0.suspention_index) == nil then
				table.remove(store.myplay_order, iter_1_0)
			end
		end

		for iter_1_1 = #store.myplay_order, 2, -1 do
			local var_1_1 = store.myplay_order[iter_1_1]

			for iter_1_2 = 1, iter_1_1 - 1 do
				local var_1_2 = store.myplay_order[iter_1_2]

				if var_1_2.game_code == var_1_1.game_code and var_1_2.suspention_index == var_1_1.suspention_index then
					table.remove(store.myplay_order, iter_1_1)

					break
				end
			end
		end

		SuspentionPoints:forEachNormal(function(arg_2_0, arg_2_1)
			if MyplayData.FindIndex(arg_2_0, arg_2_1) == var_0_0.INDEX_NOT_FOUND then
				print(string.format("myplaydata: add auto / %s: %f", arg_2_0, arg_2_1))
				MyplayData.RegisterLast(arg_2_0, arg_2_1)
			end
		end)
	end,
	IsEmpty = function()
		if #store.myplay_order == 0 then
			return true
		end

		return false
	end,
	GetTitle = function(arg_4_0)
		assert(not MyplayData.IsEmpty())

		return store.myplay_order[arg_4_0].game_code
	end,
	GetSuspentionIndex = function(arg_5_0)
		assert(not MyplayData.IsEmpty())

		return store.myplay_order[arg_5_0].suspention_index
	end,
	GetNum = function()
		return #store.myplay_order
	end,
	Register = function(arg_7_0, arg_7_1)
		MyplayData.Remove(arg_7_0, arg_7_1)
		table.insert(store.myplay_order, 1, {
			game_code = arg_7_0,
			suspention_index = arg_7_1
		})
	end,
	RegisterLast = function(arg_8_0, arg_8_1)
		MyplayData.Remove(arg_8_0, arg_8_1)
		table.insert(store.myplay_order, {
			game_code = arg_8_0,
			suspention_index = arg_8_1
		})
	end,
	Remove = function(arg_9_0, arg_9_1)
		local var_9_0 = MyplayData.FindIndex(arg_9_0, arg_9_1)

		if var_9_0 ~= var_0_0.INDEX_NOT_FOUND then
			table.remove(store.myplay_order, var_9_0)
		end
	end,
	Swap = function(arg_10_0, arg_10_1, arg_10_2)
		local var_10_0 = MyplayData.FindIndex(arg_10_0, arg_10_1)
		local var_10_1 = MyplayData.FindIndex(arg_10_0, arg_10_2)

		if var_10_0 ~= var_0_0.INDEX_NOT_FOUND and var_10_1 ~= var_0_0.INDEX_NOT_FOUND then
			store.myplay_order[var_10_0].suspention_index = arg_10_2
			store.myplay_order[var_10_1].suspention_index = arg_10_1
		elseif var_10_0 == var_0_0.INDEX_NOT_FOUND and var_10_1 == var_0_0.INDEX_NOT_FOUND then
			MyplayData.Register(arg_10_0, arg_10_2)
			print("register")
		elseif arg_10_1 == "floating" then
			MyplayData.Register(arg_10_0, arg_10_2)
		else
			if var_10_0 ~= var_0_0.INDEX_NOT_FOUND then
				store.myplay_order[var_10_0].suspention_index = arg_10_2
			end

			if var_10_1 ~= var_0_0.INDEX_NOT_FOUND then
				store.myplay_order[var_10_1].suspention_index = arg_10_1
			end
		end
	end,
	FindIndex = function(arg_11_0, arg_11_1)
		for iter_11_0, iter_11_1 in ipairs(store.myplay_order) do
			if iter_11_1.game_code == arg_11_0 and iter_11_1.suspention_index == arg_11_1 then
				return iter_11_0
			end
		end

		return var_0_0.INDEX_NOT_FOUND
	end
}
