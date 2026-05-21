SET(THREADPOOL_SOURCES
  connection_queue.cc
  thread_group.cc
  thread_pool.cc
)
ADD_LIBRARY(threadpool STATIC ${THREADPOOL_SOURCES})
ADD_DEPENDENCIES(threadpool GenError)
