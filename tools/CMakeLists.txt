if (LLVM_DG)
	set(LLVM_LINK_COMPONENTS core engine asmparser bitreader support)

	# generate a git-version.h with a HEAD commit hash tag
	# (if it changed)
	add_custom_target(
		gitversion ALL sh ${CMAKE_CURRENT_SOURCE_DIR}/git-version.sh
	)
	include_directories(${CMAKE_CURRENT_BINARY_DIR})

	add_executable(llvm-dg-dump llvm-dg-dump.cpp)
	target_link_libraries(llvm-dg-dump LLVMdg)

	add_executable(llvm-slicer llvm-slicer.cpp)
	target_link_libraries(llvm-slicer LLVMdg)
	add_dependencies(llvm-slicer gitversion)

	add_executable(llvm-ps-dump llvm-ps-dump.cpp)
	target_link_libraries(llvm-ps-dump LLVMdg)

	add_executable(llvm-pta-compare llvm-pta-compare.cpp)
	target_link_libraries(llvm-pta-compare LLVMdg)

	add_executable(llvm-rd-dump llvm-rd-dump.cpp)
	target_link_libraries(llvm-rd-dump LLVMdg)

	add_executable(llvm-to-source llvm-to-source.cpp)
	target_link_libraries(llvm-to-source PRIVATE ${llvm_libs})

	install(TARGETS llvm-dg-dump llvm-slicer
		RUNTIME DESTINATION bin)
endif (LLVM_DG)

include_directories(../src)

