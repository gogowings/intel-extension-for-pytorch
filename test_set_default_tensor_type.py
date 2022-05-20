import torch
import intel_extension_for_pytorch

# torch.xpu.set_default_tensor_type(torch.xpu.FloatTensor)
# a=torch.empty([10])
# print("a dtype {} device".format(a.dtype, a.device))

torch.xpu.set_default_tensor_type(torch.xpu.DoubleTensor)
a=torch.empty([10])
print("a dtype {} device".format(a.dtype, a.device))
