% v11-d: MATLAB rcosdesign RRC/raised-cosine references (batch-once harness for the C++ test).
names = {'rrc_025_6_4', 'rrc_05_8_4', 'rcos_025_6_4', 'rcos_05_8_4'};
hs = {rcosdesign(0.25, 6, 4, 'sqrt'), rcosdesign(0.5, 8, 4, 'sqrt'), ...
      rcosdesign(0.25, 6, 4, 'normal'), rcosdesign(0.5, 8, 4, 'normal')};
for k = 1:numel(names)
  h = hs{k};
  fprintf('inline const double ref_%s[] = {', names{k});
  for i = 1:numel(h)-1
    fprintf('%.17g, ', h(i));
  end
  fprintf('%.17g};\n', h(end));
end
exit(0);
