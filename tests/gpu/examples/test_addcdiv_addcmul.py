import torch
from torch.testing._internal.common_utils import (TestCase,
                                                  repeat_test_for_types)

cpu_device = torch.device("cpu")
xpu_device = torch.device("xpu")

class TestAddcdivAddcmul(TestCase):
    @repeat_test_for_types([torch.float, torch.int8, torch.half, torch.bfloat16])
    def test_Addcdiv(self, dtype=torch.float):
        t = torch.randn(1, 3)
        t1 = torch.randn(3, 1)
        t2 = torch.randn(1, 3)

        res = torch._foreach_addcdiv((t, ), (t1, ), (t2, ), value=0.1)
        print(res)

        t_xpu = t.to(xpu_device)
        t1_xpu = t1.to(xpu_device)
        t2_xpu = t2.to(xpu_device)
        res_xpu = torch._foreach_addcdiv((t_xpu, ), (t1_xpu, ), (t2_xpu, ), value=0.1)
        print(res_xpu)
        self.assertEqual(res, res_xpu)


    @repeat_test_for_types([torch.float, torch.int8, torch.half, torch.bfloat16])
    def test_Addcmul(self, dtype=torch.float):
        t = torch.randn(1, 3)
        t1 = torch.randn(3, 1)
        t2 = torch.randn(1, 3)

        res = torch._foreach_addcmul((t, ), (t1, ), (t2, ), value=0.1)
        print(res)

        t_xpu = t.to(xpu_device)
        t1_xpu = t1.to(xpu_device)
        t2_xpu = t2.to(xpu_device)
        res_xpu = torch._foreach_addcmul((t_xpu, ), (t1_xpu, ), (t2_xpu, ), value=0.1)
        print(res_xpu)
        self.assertEqual(res, res_xpu)
