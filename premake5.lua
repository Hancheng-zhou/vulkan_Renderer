-- ===========================================================================
--  vulkan_Renderer - premake5 构建脚本
--
--  生成工程（在仓库根目录执行）：
--      vendor\bin\premake\premake5.exe vs2022
-- ===========================================================================

workspace "vulkan_Renderer"
	architecture "x86_64"
	startproject "vulkan_Renderer"
	configurations { "Debug", "Release" }

	-- 输出目录命名，例如 Debug-windows-x86_64
	outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

	-- 公共编译选项
	filter "system:windows"
		systemversion "latest"
		staticruntime "on"
		-- /utf-8：源码统一按 UTF-8 处理
		-- /FS：强制 PDB 写入串行化
		--   【为什么必须有】下面的 multiprocessorcompile 会开多个 CL.EXE 进程，
		--   它们共用同一个 vc143.pdb。不加 /FS 时并发写会直接报
		--   "error C1041: 无法打开程序数据库"。
		--   增量编译时文件少，常常只有一个进程，所以这个问题会潜伏很久，
		--   直到某次全量重编译才突然爆出来。
		buildoptions { "/utf-8", "/FS" }
		-- 多核并行编译（/MP）。保留你 ZHC 项目里的优化配置
		multiprocessorcompile "on"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"
		-- 关掉"编辑并继续"，把 /ZI 换成 /Zi，提升链接速度
		editandcontinue "off"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter {}

-- ===========================================================================
--  第三方库
-- ===========================================================================

group "vendor"

	-- GLFW：窗口 / 输入 / 上下文
	-- 保持你 ZHC 的习惯，直接包含官方 premake 脚本进行编译
	include "vendor/GLFW"

	-- stb_image：图片解码（Vulkan 渲染纹理依然需要）
	-- 假设你已有对应的 premake5.lua 或沿用原来的结构
	include "vendor/stb_image"

	-- 注意：glm 是纯头文件库，不建工程，直接以包含目录方式使用。
	-- Vulkan 的数学计算依然可以复用 glm。

group ""

-- ===========================================================================
--  主程序
-- ===========================================================================

project "vulkan_Renderer"
	location "."
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	debugdir "$(ProjectDir)"
	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"src/**.h",
		"src/**.hpp",
		"src/**.c",
		"src/**.cpp",
	}

	includedirs
	{
		"src",
		"vendor/GLFW/include",
		"vendor/glm",
		"vendor/stb_image",
		-- 引入 Vulkan SDK 的头文件
		-- 如果你在安装时勾选了 Volk 和 VMA，这里也能一并找到
		os.getenv("VULKAN_SDK") .. "/Include",
	}

	libdirs
    {
        os.getenv("VULKAN_SDK") .. "/Lib",
    }
	-- 链接库
	links 
	{ 
		"GLFW", 
		"stb_image", 
		-- 链接 Vulkan 核心库
		"vulkan-1.lib"
	}

	filter "system:windows"
		-- Vulkan 在 Windows 上需要的系统库
		-- 保留了 ZHC 所需的 gdi32, user32, shell32，因为 GLFW 需要
		-- 新增了 ole32.lib，Vulkan 在 Windows 下通常需要它
		links { "winmm.lib", "gdi32.lib", "user32.lib", "shell32.lib", "ole32.lib" }
		
		defines
		{
			-- 启用 Vulkan 的 Windows 平台特定功能 (创建 Win32 表面必须)
			"VK_USE_PLATFORM_WIN32_KHR",
			-- 保留你 ZHC 里的 GLM 宏定义
			"GLM_ENABLE_EXPERIMENTAL",
			"GLM_FORCE_DEPTH_ZERO_TO_ONE",--让 glm::perspective 生成 z ∈ [0,1] 的投影矩阵
			"_CRT_SECURE_NO_WARNINGS",
		}

	filter {}