import pefile
import sys

pe = pefile.PE("build/Praccy.exe")
print("Sections:")
found_rsrc = False
for section in pe.sections:
    name = section.Name.decode('utf-8', errors='ignore').strip('\x00')
    print(f"  {name}: VirtualSize={section.Misc_VirtualSize}, RawSize={section.SizeOfRawData}")
    if ".rsrc" in name:
        found_rsrc = True

print(f"Has .rsrc section: {found_rsrc}")

# Check resource directory entries
if hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE'):
    for resource_type in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        type_id = resource_type.id
        type_name = str(resource_type.name) if resource_type.name else str(type_id)
        print(f"Resource Type: {type_name} (ID: {type_id})")
        if hasattr(resource_type, 'directory'):
            for resource_id in resource_type.directory.entries:
                res_id = resource_id.id
                print(f"  Resource ID: {res_id}")
                if hasattr(resource_id, 'directory'):
                    for resource_lang in resource_id.directory.entries:
                        data_rva = resource_lang.data.struct.OffsetToData
                        size = resource_lang.data.struct.Size
                        print(f"    Lang: {resource_lang.id}, Size: {size} bytes, RVA: {hex(data_rva)}")
