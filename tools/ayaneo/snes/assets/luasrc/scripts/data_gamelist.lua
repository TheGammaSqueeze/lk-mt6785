require("/scripts/titles_list.lua")
require("/scripts/suspentionpoints.lua")
require("/scripts/helper_table.lua")
require("/scripts/test_program.lua")

local var_0_0 = 30

GameList = class()

function GameList.loadThumbnail(arg_1_0)
	GameList.order = {}

	local var_1_0 = {}

	if titles_list then
		for iter_1_0, iter_1_1 in pairs(titles_list) do
			local var_1_1 = iter_1_1.game_code

			if var_1_1:find("PRODUCTION") then
				test_program.info = iter_1_1
			else
				var_1_0[var_1_1] = iter_1_1

				if store.info[var_1_1] == nil then
					store.info[var_1_1] = {}
				end

				local var_1_2 = FileUtils.cleanupPath(iter_1_1.thumbnail)
				local var_1_3 = createImageFromPath(var_1_2)

				var_1_3:link()

				iter_1_1.spriteResource = var_1_3

				local var_1_4 = var_1_2:match("^(.*)/(.*)%.png$") .. "/" .. var_1_1 .. "_small.png"
				local var_1_5 = createImageFromPath(var_1_4)

				var_1_5:link()

				iter_1_1.iconResource = var_1_5

				if iter_1_1.autoplay then
					table.sort(iter_1_1.autoplay)
				end

				if iter_1_1.autoplay then
					table.sort(iter_1_1.superplay)
				end

				if iter_1_1.pixel_image then
					table.sort(iter_1_1.pixel_image)
				end

				table.insert(GameList.order, iter_1_1)
			end
		end
	end

	titles_list = var_1_0

	if store.autoplay and store.autoplay.running then
		GameList:sortOrder(1)
	else
		GameList:sortOrder(store.sortrule or 1)
	end
end

function GameList.limitTitleNum(arg_2_0)
	local var_2_0

	if system.is_hvc() then
		var_2_0 = "J"
	end

	if system.is_nes() then
		var_2_0 = "E"
	end

	system.titlies_list_original = titles_list

	local var_2_1 = {}
	local var_2_2 = {}
	local var_2_3 = 0

	for iter_2_0, iter_2_1 in pairs(titles_list) do
		local var_2_4 = iter_2_1.game_code

		if var_2_4:find("PRODUCTION") then
			var_2_1[iter_2_0] = iter_2_1
		else
			if var_2_4:sub(-1, -1) == var_2_0 then
				var_2_1[iter_2_0] = iter_2_1
				var_2_3 = var_2_3 + 1
			else
				var_2_2[iter_2_0] = iter_2_1
			end

			if var_2_3 >= var_0_0 then
				break
			end
		end
	end

	return var_2_1
end

local var_0_1 = 2

local function var_0_2(arg_3_0, arg_3_1)
	return arg_3_0.sort_raw_title < arg_3_1.sort_raw_title
end

local function var_0_3(arg_4_0)
	return arg_4_0.sort_raw_title
end

local function var_0_4(arg_5_0, arg_5_1)
	local var_5_0 = math.floor(arg_5_0.players)
	local var_5_1 = math.floor(arg_5_1.players)

	if var_5_0 > var_0_1 then
		var_5_0 = var_0_1
	end

	if var_5_1 > var_0_1 then
		var_5_1 = var_0_1
	end

	if var_5_0 ~= var_5_1 then
		return var_5_1 < var_5_0
	else
		local var_5_2 = arg_5_0.simultaneous
		local var_5_3 = arg_5_1.simultaneous

		if var_5_2 ~= var_5_3 then
			return var_5_3 < var_5_2
		else
			return var_0_2(arg_5_0, arg_5_1)
		end
	end
end

local function var_0_5(arg_6_0)
	local var_6_0

	return (arg_6_0.players == 1 and 1 or 2) .. "\n" .. arg_6_0.sort_raw_title
end

local function var_0_6(arg_7_0, arg_7_1)
	local var_7_0 = table_wrapNullable
	local var_7_1 = table_unwrapNullable
	local var_7_2 = var_7_1(var_7_0(store).info[arg_7_0.game_code].recently_number) or 0
	local var_7_3 = var_7_1(var_7_0(store).info[arg_7_1.game_code].recently_number) or 0

	if var_7_2 ~= var_7_3 then
		return var_7_3 < var_7_2
	else
		return var_0_2(arg_7_0, arg_7_1)
	end
end

local function var_0_7(arg_8_0)
	local var_8_0 = table_wrapNullable

	return (table_unwrapNullable(var_8_0(store).info[arg_8_0.game_code].recently_number) or 0) .. "\n" .. arg_8_0.sort_raw_title
end

local function var_0_8(arg_9_0, arg_9_1)
	local var_9_0 = table_wrapNullable
	local var_9_1 = table_unwrapNullable
	local var_9_2 = var_9_1(var_9_0(store).info[arg_9_0.game_code].play_count) or 0
	local var_9_3 = var_9_1(var_9_0(store).info[arg_9_1.game_code].play_count) or 0

	if var_9_2 ~= var_9_3 then
		return var_9_3 < var_9_2
	else
		return var_0_2(arg_9_0, arg_9_1)
	end
end

local function var_0_9(arg_10_0)
	local var_10_0 = table_wrapNullable

	return (table_unwrapNullable(var_10_0(store).info[arg_10_0.game_code].play_count) or 0) .. "\n" .. arg_10_0.sort_raw_title
end

local function var_0_10(arg_11_0, arg_11_1)
	local var_11_0 = arg_11_0.release_date
	local var_11_1 = arg_11_1.release_date

	if var_11_0 ~= var_11_1 then
		return var_11_0 < var_11_1
	else
		return var_0_2(arg_11_0, arg_11_1)
	end
end

local function var_0_11(arg_12_0)
	return arg_12_0.release_date .. "\n" .. arg_12_0.sort_raw_title
end

local function var_0_12(arg_13_0, arg_13_1)
	local var_13_0 = arg_13_0.sort_raw_publisher
	local var_13_1 = arg_13_1.sort_raw_publisher

	if var_13_0 ~= var_13_1 then
		return var_13_0 < var_13_1
	else
		return var_0_2(arg_13_0, arg_13_1)
	end
end

local function var_0_13(arg_14_0)
	return arg_14_0.sort_raw_publisher .. "\n" .. arg_14_0.sort_raw_title
end

local var_0_14 = {
	{
		label = "sys_sort_order_TitleName",
		name = "name",
		func = var_0_2,
		dbgtext = var_0_3
	},
	{
		label = "sys_sort_order_Players",
		name = "players",
		func = var_0_4,
		dbgtext = var_0_5
	},
	{
		label = "sys_sort_order_RecentPlay",
		name = "recent",
		func = var_0_6,
		dbgtext = var_0_7
	},
	{
		label = "sys_sort_order_PlayCount",
		name = "playcount",
		func = var_0_8,
		dbgtext = var_0_9
	},
	{
		label = "sys_sort_order_ReleaseDate",
		name = "release",
		func = var_0_10,
		dbgtext = var_0_11
	},
	{
		label = "sys_sort_order_Manufacturer",
		name = "publisher",
		func = var_0_12,
		dbgtext = var_0_13
	}
}

function GameList.sortOrder(arg_15_0, arg_15_1)
	local var_15_0 = arg_15_1 or 1

	if var_15_0 < 1 then
		var_15_0 = 1
	end

	if var_15_0 > #var_0_14 then
		var_15_0 = var_0_14
	end

	local var_15_1 = var_0_14[var_15_0]
	local var_15_2 = Localization.getText(var_15_1.label)

	debugLabelPrint(var_15_2)

	local var_15_3 = var_15_1.dbgtext

	table.sort(GameList.order, var_15_1.func)

	return GameList.order, var_15_2, var_15_3
end

function GameList.getSortRuleMax(arg_16_0)
	return #var_0_14
end
