SET(THREADPOOL_SOURCES
  thread_pool.cc
  thread_group.cc
)
ADD_LIBRARY(threadpool STATIC ${THREADPOOL_SOURCES})
