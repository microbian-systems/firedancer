$(call add-hdrs,fd_bmtree.h)
$(call add-objs,fd_bmtree,fd_ballet)
$(call make-unit-test,test_bmtree,test_bmtree,fd_ballet fd_util)
$(call run-unit-test,test_bmtree)
