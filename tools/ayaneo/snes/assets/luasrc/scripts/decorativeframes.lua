DecorativeFrames = class()
DecorativeFrames.texture_types = {
	THUMBNAIL = 3,
	PREVIEW_PIXEL_PERFECT = 2,
	PREVIEW_4_3 = 1
}
DecorativeFrames.suffixes = {
	PIXEL_PERFECT = "_pixel_perfect",
	THUMBNAIL = "_thumbnail",
	["4_3"] = "_4_3",
	PREVIEW_PIXEL_PERFECT = "_pixel_perfect_preview",
	PREVIEW_4_3 = "_4_3_preview"
}
DecorativeFrames.texture_type_to_suffix = {
	[DecorativeFrames.texture_types.PREVIEW_4_3] = DecorativeFrames.suffixes.PREVIEW_4_3,
	[DecorativeFrames.texture_types.PREVIEW_PIXEL_PERFECT] = DecorativeFrames.suffixes.PREVIEW_PIXEL_PERFECT,
	[DecorativeFrames.texture_types.THUMBNAIL] = DecorativeFrames.suffixes.THUMBNAIL
}
DecorativeFrames.display_type_to_preview_type = {
	["pixel-perfect"] = DecorativeFrames.texture_types.PREVIEW_PIXEL_PERFECT,
	["keep-aspect-ratio"] = DecorativeFrames.texture_types.PREVIEW_4_3,
	["crt-filter"] = DecorativeFrames.texture_types.PREVIEW_4_3
}
DecorativeFrames.display_type_to_suffix = {
	["pixel-perfect"] = DecorativeFrames.suffixes.PIXEL_PERFECT,
	["keep-aspect-ratio"] = DecorativeFrames.suffixes["4_3"],
	["crt-filter"] = DecorativeFrames.suffixes["4_3"]
}

function DecorativeFrames.init(arg_1_0)
	local var_1_0 = arg_1_0:enumerateFrames(DECORATIVE_FRAMES_PATH)

	arg_1_0.frames = {}

	for iter_1_0, iter_1_1 in pairs(var_1_0) do
		local var_1_1 = {
			frame_name = iter_1_1,
			textures = {}
		}

		for iter_1_2, iter_1_3 in pairs(arg_1_0.texture_type_to_suffix) do
			var_1_1.textures[iter_1_2] = arg_1_0:linkFrameResource(DECORATIVE_FRAMES_PATH, iter_1_1, iter_1_3)
		end

		arg_1_0.frames[#arg_1_0.frames + 1] = var_1_1
	end
end

function DecorativeFrames.enumerateFrames(arg_2_0, arg_2_1)
	local var_2_0 = FileUtils.listDirectories(arg_2_1)

	table.sort(var_2_0)

	local var_2_1 = {}

	for iter_2_0, iter_2_1 in pairs(var_2_0) do
		if iter_2_1:sub(1, 1) ~= "." and arg_2_0:checkFrameFolder(arg_2_0:getFrameFolderPath(arg_2_1, iter_2_1), iter_2_1) then
			var_2_1[#var_2_1 + 1] = iter_2_1
		end
	end

	return var_2_1
end

function DecorativeFrames.checkFrameFolder(arg_3_0, arg_3_1, arg_3_2)
	for iter_3_0, iter_3_1 in pairs(arg_3_0.suffixes) do
		if not FileUtils.fileExists(arg_3_1 .. arg_3_0:getFrameFileName(arg_3_2, iter_3_1)) then
			return false
		end
	end

	return true
end

function DecorativeFrames.linkFrameResource(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
	local var_4_0 = createImageFromPath(arg_4_0:getFramePath(arg_4_1, arg_4_2, arg_4_3))

	var_4_0:link()

	return var_4_0
end

function DecorativeFrames.getFrameCount(arg_5_0)
	return #arg_5_0.frames
end

function DecorativeFrames.getThumbnailTexture(arg_6_0, arg_6_1)
	return arg_6_0.frames[arg_6_1].textures[arg_6_0.texture_types.THUMBNAIL]
end

function DecorativeFrames.getPreviewTexture(arg_7_0, arg_7_1, arg_7_2)
	local var_7_0 = arg_7_0.display_type_to_preview_type[arg_7_2]

	return arg_7_0.frames[arg_7_1].textures[var_7_0]
end

function DecorativeFrames.getFullFrameName(arg_8_0, arg_8_1, arg_8_2)
	local var_8_0 = arg_8_0.frames[arg_8_1].frame_name

	return var_8_0 .. "/" .. var_8_0 .. arg_8_0.display_type_to_suffix[arg_8_2]
end

function DecorativeFrames.getFrameFolderPath(arg_9_0, arg_9_1, arg_9_2)
	return arg_9_1 .. "/" .. arg_9_2 .. "/"
end

function DecorativeFrames.getFrameFileName(arg_10_0, arg_10_1, arg_10_2)
	return arg_10_1 .. arg_10_2 .. ".png"
end

function DecorativeFrames.getFramePath(arg_11_0, arg_11_1, arg_11_2, arg_11_3)
	return arg_11_0:getFrameFolderPath(arg_11_1, arg_11_2) .. arg_11_0:getFrameFileName(arg_11_2, arg_11_3)
end

function DecorativeFrames.isLoading(arg_12_0, arg_12_1)
	local var_12_0 = arg_12_0.frames[arg_12_1]

	for iter_12_0, iter_12_1 in pairs(arg_12_0.texture_types) do
		if var_12_0.textures[iter_12_1]:isLoading() then
			return true
		end
	end

	return false
end

function DecorativeFrames.hasLoadError(arg_13_0, arg_13_1)
	local var_13_0 = arg_13_0.frames[arg_13_1]

	for iter_13_0, iter_13_1 in pairs(arg_13_0.texture_types) do
		if var_13_0.textures[iter_13_1]:hasLoadError() then
			return true
		end
	end

	return false
end

function DecorativeFrames.areLoading(arg_14_0)
	for iter_14_0 in pairs(arg_14_0.frames) do
		if arg_14_0:isLoading(iter_14_0) then
			return true
		end
	end

	return false
end

function DecorativeFrames.checkLoadErrors(arg_15_0)
	local var_15_0 = 1

	while var_15_0 <= #arg_15_0.frames do
		if arg_15_0:hasLoadError(var_15_0) then
			table.remove(arg_15_0.frames, var_15_0)
		else
			var_15_0 = var_15_0 + 1
		end
	end
end
