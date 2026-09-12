#include "sys.h"
#include "Application.h"

std::ofstream Application::log_;
std::mutex Application::log_mutex_;
