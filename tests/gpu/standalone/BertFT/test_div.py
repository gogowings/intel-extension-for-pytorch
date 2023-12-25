import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
xpu_device = torch.device("xpu")

shapes = [
        (2, 16, 384, 384)
]

class TestTorchMethod(TestCase):
    def test_div(self, dtype=torch.float):
        for shape in shapes:
            print("\n================== test shape: ", shape, "==================")
            user_cpu = torch.randn(shape, device=cpu_device)
            res_cpu = torch.div(user_cpu, 2)
            print("begin xpu compute:")
            res_xpu = torch.div(user_cpu.to("xpu"), 2)
            print("xpu result:")
            print(res_xpu.cpu())
            self.assertEqual(res_cpu, res_xpu.cpu())
