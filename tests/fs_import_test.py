print("fs import")
import utils
print(utils.answer)
try:
    import missing_module
except ModuleNotFoundError as e:
    print("missing caught")
